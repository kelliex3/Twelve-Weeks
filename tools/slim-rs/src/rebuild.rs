use std::fs::{self, File, OpenOptions};
use std::io::{self, BufRead, BufReader, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};

use crate::archive::{game_archive, valid_archive_name, Dsar, Psarc};
use crate::format::*;
use crate::util::*;

const GROUP: usize = 64;
const GAME_VERSION: &str = "Your game version differs from the one this update was made for (The Last of Us Part II Remastered v1.6.10721, Steam). Verify the game files in Steam and try again.";

pub struct Progress {
    total: u64,
    done: u64,
    last: i64,
}

impl Progress {
    pub fn new() -> Progress {
        Progress { total: 0, done: 0, last: -1 }
    }

    fn add(&mut self, n: u64) {
        self.done += n;
        if self.total == 0 {
            return;
        }
        let d = self.done.min(self.total);
        let pct = (d * 100 / self.total) as i64;
        if pct != self.last {
            self.last = pct;
            println!("t2r-slim: progress {} {}", d, self.total);
            let _ = io::stdout().flush();
        }
    }
}

struct TmpGuard(Option<PathBuf>);

impl Drop for TmpGuard {
    fn drop(&mut self) {
        if let Some(p) = self.0.take() {
            let _ = fs::remove_file(p);
        }
    }
}

pub struct Slim {
    pub f: File,
    pub size: u64,
    pub head: Head,
    pub recs: Vec<Rec>,
    pub chunks: Vec<Chunk>,
    pub lits: Vec<Lit>,
}

fn table(f: &File, at: &mut u64, n: usize, each: usize, file_size: u64, what: &str) -> Res<Vec<u8>> {
    let len = (n as u64).checked_mul(each as u64).filter(|l| at.checked_add(*l).map_or(false, |e| e <= file_size));
    let len = match len {
        Some(l) => l as usize,
        None => return fail(EXIT_IO, format!("the slim data file is truncated ({})", what)),
    };
    let mut b = vec![0u8; len];
    read_at(f, *at, &mut b)?;
    *at += len as u64;
    Ok(b)
}

impl Slim {
    pub fn open(path: &Path) -> Res<Slim> {
        let f = io_ctx(File::open(path), || format!("cannot open {}", path.display()))?;
        let size = f.metadata()?.len();
        let mut hb = vec![0u8; HEAD_SIZE];
        if size < HEAD_SIZE as u64 {
            return fail(EXIT_IO, format!("{} is not a slim data file", path.display()));
        }
        read_at(&f, 0, &mut hb)?;
        let head = Head::from_bytes(&hb)?;
        if head.nrec > 1 << 24 || head.nchunks > 1 << 26 || head.nlit > 1 << 24 {
            return fail(EXIT_IO, "the slim data file is damaged (table sizes)");
        }
        let mut at = HEAD_SIZE as u64;
        let rb = table(&f, &mut at, head.nrec as usize, REC_SIZE, size, "records")?;
        let cb = table(&f, &mut at, head.nchunks as usize, CHUNK_SIZE, size, "chunks")?;
        let lb = table(&f, &mut at, head.nlit as usize, LIT_SIZE, size, "literals")?;
        let s = Slim {
            recs: rb.chunks_exact(REC_SIZE).map(Rec::from_bytes).collect(),
            chunks: cb.chunks_exact(CHUNK_SIZE).map(Chunk::from_bytes).collect(),
            lits: lb.chunks_exact(LIT_SIZE).map(Lit::from_bytes).collect(),
            f,
            size,
            head,
        };
        s.validate()?;
        Ok(s)
    }

    fn validate(&self) -> Res<()> {
        let h = &self.head;
        let bad = |m: String| fail::<()>(EXIT_IO, format!("the slim data file is damaged: {}", m));
        if h.blob_off % 8 != 0 || h.blob_off > h.out_size || h.blob_size > h.out_size - h.blob_off {
            return bad("archive placement".into());
        }
        let in_file = |off: u64, len: u64| off.checked_add(len).map_or(false, |e| e <= self.size);
        if h.pack_len > 0 && !in_file(h.pack_off, h.pack_len) {
            return bad("pack stream outside the file".into());
        }
        for (i, c) in self.chunks.iter().enumerate() {
            if c.usz as usize > DSAR_CHUNK || c.payload_off.checked_add(c.csz as u64).map_or(true, |e| e > h.blob_size) {
                return bad(format!("chunk {}", i));
            }
        }
        for (i, l) in self.lits.iter().enumerate() {
            if l.out_off.checked_add(l.len).map_or(true, |e| e > h.out_size) || !in_file(l.dat_off, l.len) {
                return bad(format!("literal range {}", i));
            }
        }
        let mut pack_pos = 0u64;
        for (i, r) in self.recs.iter().enumerate() {
            if r.chunk0 as u64 + r.nchunks as u64 > self.chunks.len() as u64 {
                return bad(format!("record {} chunk range", i));
            }
            let usz: u64 = self.chunks[r.chunk0 as usize..(r.chunk0 + r.nchunks) as usize].iter().map(|c| c.usz as u64).sum();
            if usz != r.usz {
                return bad(format!("record {} size", i));
            }
            match r.kind {
                KIND_GAME => {
                    if !valid_archive_name(&r.arch) || (r.delta_len > 0 && !in_file(r.delta_off, r.delta_len)) {
                        return bad(format!("record {}", i));
                    }
                }
                KIND_PACK => {
                    if r.pack_pos != pack_pos {
                        return bad(format!("record {} pack order", i));
                    }
                    pack_pos += r.usz;
                }
                _ => return bad(format!("record {} kind", i)),
            }
        }
        if pack_pos != h.pack_usz || (h.pack_len == 0) != (pack_pos == 0) {
            return bad("pack stream size".into());
        }
        Ok(())
    }
}

fn threads() -> usize {
    let n = std::env::var("T2R_SLIM_THREADS").ok().and_then(|v| v.parse().ok()).unwrap_or_else(|| std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4));
    n.clamp(1, 16)
}

pub fn write_chunks(out: &File, head: &Head, rec: &Rec, chunks: &[Chunk], res: &[u8]) -> Res<()> {
    let cks = &chunks[rec.chunk0 as usize..(rec.chunk0 + rec.nchunks) as usize];
    let ks_end = head.blob_size & !7;
    let nt = threads();
    for (g, group) in cks.chunks(GROUP).enumerate() {
        let base = g * GROUP;
        let next = AtomicUsize::new(0);
        let done: Vec<(usize, Option<Vec<u8>>)> = std::thread::scope(|s| {
            let hs: Vec<_> = (0..nt.min(group.len()))
                .map(|_| {
                    s.spawn(|| {
                        let mut mine = Vec::new();
                        loop {
                            let i = next.fetch_add(1, Ordering::Relaxed);
                            if i >= group.len() {
                                break mine;
                            }
                            let a = (base + i) * DSAR_CHUNK;
                            let b = a + group[i].usz as usize;
                            mine.push((i, if b <= res.len() { lz4_compress_chunk(&res[a..b]) } else { None }));
                        }
                    })
                })
                .collect();
            hs.into_iter().flat_map(|h| h.join().unwrap_or_default()).collect()
        });
        let mut outs: Vec<Option<Vec<u8>>> = vec![None; group.len()];
        for (i, r) in done {
            outs[i] = r;
        }
        for (i, o) in outs.into_iter().enumerate() {
            let c = group[i];
            let mut buf = match o {
                Some(b) => b,
                None => return fail(EXIT_INTERNAL, format!("{}: chunk {} could not be compressed", rec.path, base + i)),
            };
            if buf.len() != c.csz as usize {
                return fail(EXIT_INTERNAL, format!("{}: compressed size of chunk {} is {}, expected {} (lz4 differs from the build's)", rec.path, base + i, buf.len(), c.csz));
            }
            xor_keystream(&mut buf, c.payload_off, ks_end);
            io_ctx(write_at(out, head.blob_off + c.payload_off, &buf), || "writing the output failed (disk full?)".into())?;
        }
    }
    Ok(())
}

fn zstd_patch_apply(base: &[u8], delta: &[u8], usz: usize) -> Option<Vec<u8>> {
    let mut d = zstd_safe::DCtx::create();
    d.set_parameter(zstd_safe::DParameter::WindowLogMax(31)).ok()?;
    d.ref_prefix(base).ok()?;
    let mut out: Vec<u8> = Vec::with_capacity(usz);
    match d.decompress(&mut out, delta) {
        Ok(n) if n == usz && out.len() == usz => Some(out),
        _ => None,
    }
}

pub fn rebuild(slim_path: &Path, game: &Path, out_path: &Path) -> Res<()> {
    let s = Slim::open(slim_path)?;
    let h = &s.head;
    let mut tmp_os = out_path.as_os_str().to_owned();
    tmp_os.push(".tmp");
    let tmp = PathBuf::from(tmp_os);
    let mut guard = TmpGuard(Some(tmp.clone()));
    let out = io_ctx(OpenOptions::new().read(true).write(true).create(true).truncate(true).open(&tmp), || format!("cannot create {} (is the game folder writable?)", tmp.display()))?;
    io_ctx(out.set_len(h.out_size), || format!("cannot create {} (disk full?)", tmp.display()))?;
    let mut prog = Progress::new();
    prog.total = s.lits.iter().map(|l| l.len).sum::<u64>() + s.recs.iter().map(|r| r.usz).sum::<u64>() + h.out_size;
    prog.add(0);

    let mut buf = vec![0u8; 1 << 20];
    for l in &s.lits {
        let mut done = 0u64;
        while done < l.len {
            let n = (l.len - done).min(buf.len() as u64) as usize;
            read_at(&s.f, l.dat_off + done, &mut buf[..n])?;
            io_ctx(write_at(&out, l.out_off + done, &buf[..n]), || "writing the output failed (disk full?)".into())?;
            done += n as u64;
            prog.add(n as u64);
        }
    }

    let mut order: Vec<usize> = (0..s.recs.len()).filter(|&i| s.recs[i].kind == KIND_GAME).collect();
    order.sort_by(|&a, &b| s.recs[a].arch.cmp(&s.recs[b].arch));
    let mut open: Option<(String, Psarc<Dsar>)> = None;
    for i in order {
        let r = &s.recs[i];
        if open.as_ref().map(|o| o.0 != r.arch).unwrap_or(true) {
            let ap = game_archive(game, &r.arch);
            let p = match Dsar::open(&ap).and_then(|d| Psarc::open(d, &r.arch)) {
                Ok(p) => p,
                Err(e) => return fail(EXIT_GAME, format!("cannot read the game archive {} (needed for {}): {}. {}", ap.display(), r.path, e, GAME_VERSION)),
            };
            open = Some((r.arch.clone(), p));
        }
        let p = &mut open.as_mut().unwrap().1;
        let base = match p.file(&r.md5, &r.arch) {
            Ok(Some(b)) => b,
            Ok(None) => return fail(EXIT_GAME, format!("{}: not found in {}. {}", r.path, r.arch, GAME_VERSION)),
            Err(e) => return fail(EXIT_GAME, format!("{}: {}. {}", r.path, e, GAME_VERSION)),
        };
        if base.len() as u64 != r.base_size {
            return fail(EXIT_GAME, format!("{}: the game's file has {} bytes, expected {}. {}", r.path, base.len(), r.base_size, GAME_VERSION));
        }
        let res = if r.delta_len > 0 {
            let mut delta = vec![0u8; r.delta_len as usize];
            read_at(&s.f, r.delta_off, &mut delta)?;
            match zstd_patch_apply(&base, &delta, r.usz as usize) {
                Some(v) => v,
                None => return fail(EXIT_GAME, format!("{}: the patch does not fit the game's file. {}", r.path, GAME_VERSION)),
            }
        } else {
            base
        };
        if res.len() as u64 != r.usz || fnv(FNV_INIT, &res) != r.raw_fnv {
            return fail(EXIT_GAME, format!("{}: result hash mismatch. {}", r.path, GAME_VERSION));
        }
        write_chunks(&out, h, r, &s.chunks, &res)?;
        prog.add(r.usz);
    }
    drop(open);

    if h.pack_len > 0 {
        let mut src = s.f.try_clone()?;
        src.seek(SeekFrom::Start(h.pack_off))?;
        let mut dec = liblzma::bufread::XzDecoder::new_multi_decoder(BufReader::with_capacity(1 << 20, src.take(h.pack_len)));   // one xz stream per pack group (make.rs pack_groups); an old single-stream pack decodes the same
        let mut n_files = 0u32;
        for r in s.recs.iter().filter(|r| r.kind == KIND_PACK) {
            let mut res = vec![0u8; r.usz as usize];
            if dec.read_exact(&mut res).is_err() {
                return fail(EXIT_IO, format!("{}: the pack stream is damaged or ends early (damaged download?)", r.path));
            }
            if fnv(FNV_INIT, &res) != r.raw_fnv {
                return fail(EXIT_INTERNAL, format!("{}: unpacked data does not match its checksum (damaged download?)", r.path));
            }
            write_chunks(&out, h, r, &s.chunks, &res)?;
            prog.add(r.usz);
            n_files += 1;
        }
        let mut one = [0u8; 1];
        match dec.read(&mut one) {
            Ok(0) => {}
            Ok(_) => return fail(EXIT_INTERNAL, "the pack stream holds more data than expected"),
            Err(_) => return fail(EXIT_IO, "the pack stream end is damaged (damaged download?)"),
        }
        let rest = dec.get_mut();
        if !rest.fill_buf()?.is_empty() {
            return fail(EXIT_INTERNAL, "trailing bytes after the pack stream");
        }
        println!("t2r-slim: unpacked {} files", n_files);
    }

    let mut hf = FNV_INIT;
    let mut hp = FNV_INIT;
    let (pa, pb) = (h.blob_off, h.blob_off + h.blob_size);
    let mut rb = vec![0u8; 1 << 22];
    let mut pos = 0u64;
    while pos < h.out_size {
        let n = (h.out_size - pos).min(rb.len() as u64) as usize;
        read_at(&out, pos, &mut rb[..n])?;
        hf = fnv(hf, &rb[..n]);
        let (a, b) = (pos.max(pa), (pos + n as u64).min(pb));
        if a < b {
            hp = fnv(hp, &rb[(a - pos) as usize..(b - pos) as usize]);
        }
        pos += n as u64;
        prog.add(n as u64);
    }
    if hp != h.blob_fnv {
        return fail(EXIT_INTERNAL, format!("the rebuilt mod archive does not match its checksum ({:016x} != {:016x})", hp, h.blob_fnv));
    }
    if hf != h.out_fnv {
        return fail(EXIT_INTERNAL, format!("the rebuilt file does not match its checksum ({:016x} != {:016x})", hf, h.out_fnv));
    }
    io_ctx(out.sync_all(), || "writing the output failed (disk full?)".into())?;
    drop(out);
    io_ctx(fs::rename(&tmp, out_path), || format!("cannot replace {}. Is the game running?", out_path.display()))?;
    guard.0 = None;
    println!("t2r-slim: {} written ({} bytes), checksum ok", out_path.display(), h.out_size);
    Ok(())
}
