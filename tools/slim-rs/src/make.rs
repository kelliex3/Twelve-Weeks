use std::collections::{HashMap, HashSet};
use std::fs::{self, File};
use std::io::{BufWriter, Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::Instant;

use sha2::{Digest, Sha256};

use crate::archive::{game_archive, path_md5, Dsar, GameIndex, Psarc};
use crate::format::*;
use crate::moddata::ModData;
use crate::util::*;

pub struct MakeOpts {
    pub data: PathBuf,
    pub stage: PathBuf,
    pub game: PathBuf,
    pub out: PathBuf,
    pub cache: Option<PathBuf>,
    pub jobs: usize,
    pub xz_threads: u32,
}

struct Item {
    path: String,
    fp: PathBuf,
    size: u64,
    chunks: Vec<usize>,
    game: Option<(String, u64)>,
}

struct Done {
    delta: Option<Vec<u8>>,
    raw_fnv: u64,
}

fn walk(dir: &Path, rel: &str, out: &mut Vec<(String, PathBuf)>) -> Res<()> {
    let mut entries: Vec<_> = io_ctx(fs::read_dir(dir), || format!("cannot list {}", dir.display()))?.filter_map(|e| e.ok()).collect();
    entries.sort_by_key(|e| e.file_name());
    for e in entries {
        let name = e.file_name().to_string_lossy().into_owned();
        let r = if rel.is_empty() { name.clone() } else { format!("{}/{}", rel, name) };
        let ft = e.file_type()?;
        if ft.is_dir() {
            walk(&e.path(), &r, out)?;
        } else if ft.is_file() {
            out.push((r, e.path()));
        }
    }
    Ok(())
}

fn window_log(n: usize) -> u32 {
    let mut w = 20;
    while w < 31 && (1usize << w) < n {
        w += 1;
    }
    w
}

fn zstd_delta(base: &[u8], target: &[u8]) -> Res<Vec<u8>> {
    use zstd_safe::CParameter as P;
    let level = if target.len() <= 24 << 20 { 19 } else { 12 };
    let err = |e: usize| Fail { code: EXIT_INTERNAL, msg: format!("zstd: {}", zstd_safe::get_error_name(e)) };
    let mut c = zstd_safe::CCtx::create();
    c.set_parameter(P::CompressionLevel(level)).map_err(err)?;
    c.set_parameter(P::EnableLongDistanceMatching(true)).map_err(err)?;
    c.set_parameter(P::WindowLog(window_log(base.len() + target.len()))).map_err(err)?;
    c.set_parameter(P::ChecksumFlag(true)).map_err(err)?;
    c.ref_prefix(base).map_err(err)?;
    let mut dst: Vec<u8> = Vec::with_capacity(zstd_safe::compress_bound(target.len()));
    c.compress2(&mut dst, target).map_err(err)?;
    Ok(dst)
}

fn sha_hex(b: &[u8]) -> String {
    Sha256::digest(b).iter().map(|x| format!("{:02x}", x)).collect()
}

fn delta_cached(cache: Option<&Path>, base: &[u8], target: &[u8]) -> Res<Vec<u8>> {
    let key = cache.map(|c| c.join(format!("{}-{}-v1.zst", sha_hex(target), sha_hex(base))));
    if let Some(k) = &key {
        if let Ok(d) = fs::read(k) {
            return Ok(d);
        }
    }
    let d = zstd_delta(base, target)?;
    if let Some(k) = &key {
        let tmp = k.with_extension(format!("tmp{}", std::process::id()));
        if fs::write(&tmp, &d).is_ok() {
            let _ = fs::rename(&tmp, k);
        }
    }
    Ok(d)
}

fn verify_chunks(md: &ModData, it: &Item, staged: &[u8]) -> Res<()> {
    for (k, &c) in it.chunks.iter().enumerate() {
        let a = k * DSAR_CHUNK;
        let b = (a + DSAR_CHUNK).min(staged.len());
        if md.chunk(c)? != staged[a..b] {
            return fail(EXIT_INTERNAL, format!("{}: chunk {} of t2r-data.bin differs from the staged file (stage and data file are from different builds?)", it.path, c));
        }
    }
    Ok(())
}

fn work(md: &ModData, it: &Item, game: &Path, cache: Option<&Path>, archives: &mut HashMap<String, Psarc<Dsar>>) -> Res<Done> {
    let staged = io_ctx(fs::read(&it.fp), || format!("cannot read {}", it.fp.display()))?;
    if staged.len() as u64 != it.size {
        return fail(EXIT_INTERNAL, format!("{} changed while the slim file was made", it.path));
    }
    verify_chunks(md, it, &staged)?;
    let raw_fnv = fnv(FNV_INIT, &staged);
    let delta = match &it.game {
        None => None,
        Some((arch, bsz)) => {
            if !archives.contains_key(arch) {
                if archives.len() >= 4 {
                    archives.clear();
                }
                archives.insert(arch.clone(), Psarc::open(Dsar::open(&game_archive(game, arch))?, arch)?);
            }
            let base = match archives.get_mut(arch).unwrap().file(&path_md5(&it.path), arch)? {
                Some(b) => b,
                None => return fail(EXIT_GAME, format!("{} vanished from {}", it.path, arch)),
            };
            if base.len() as u64 != *bsz {
                return fail(EXIT_GAME, format!("{}: game file size changed", it.path));
            }
            if base == staged {
                Some(Vec::new())
            } else {
                Some(delta_cached(cache, &base, &staged)?)
            }
        }
    };
    Ok(Done { delta, raw_fnv })
}

// Pack groups (patch-sized updates): the pack is a run of independent xz streams, one per group of consecutive pack files. A group ends
// after a file whose path hash hits GROUP_CUT_MOD once the group holds GROUP_MIN bytes, or when it reaches GROUP_MAX. The cut points
// depend on paths (and sizes only through the caps), so between two builds every group whose files did not change compresses to
// the same bytes, only at another offset; the launcher's content-defined patch (make_patch.py file) then carries only the changed
// groups. One xz stream for the whole pack made every byte after the first change differ, so any data change was a full download.
pub const GROUP_MIN: u64 = 1 << 20;
pub const GROUP_MAX: u64 = 8 << 20;
pub const GROUP_CUT_MOD: u64 = 4;

pub fn pack_groups(paths_sizes: &[(&str, u64)]) -> Vec<(usize, usize)> {
    let mut out = Vec::new();
    let (mut start, mut acc) = (0usize, 0u64);
    for (i, (p, sz)) in paths_sizes.iter().enumerate() {
        acc += *sz;
        let cut = (acc >= GROUP_MIN && path_md5(p)[0] as u64 % GROUP_CUT_MOD == 0) || acc >= GROUP_MAX;   // md5: fnv here hashes 8-byte words, its low bits barely move between similar paths
        if cut || i + 1 == paths_sizes.len() {
            out.push((start, i + 1));
            start = i + 1;
            acc = 0;
        }
    }
    out
}

fn xz_group(items: &[&Item]) -> Res<(Vec<u8>, u64)> {
    use liblzma::stream::{Check, Filters, LzmaOptions, MatchFinder, Stream, PRESET_EXTREME};
    let mut raw = Vec::new();
    for it in items {
        let mut f = io_ctx(File::open(&it.fp), || format!("cannot read {}", it.fp.display()))?;
        f.read_to_end(&mut raw)?;
    }
    let mut dict = 1u32 << 20;
    while (dict as u64) < raw.len() as u64 && dict < (64 << 20) {
        dict <<= 1;
    }
    let mut opts = LzmaOptions::new_preset(9 | PRESET_EXTREME).map_err(|e| Fail { code: EXIT_INTERNAL, msg: format!("xz: {}", e) })?;
    opts.dict_size(dict).literal_context_bits(1).literal_position_bits(3).position_bits(3).nice_len(273).match_finder(MatchFinder::BinaryTree4).depth(1000);
    let mut filters = Filters::new();
    filters.lzma2(&opts);
    let stream = Stream::new_stream_encoder(&filters, Check::Crc64).map_err(|e| Fail { code: EXIT_INTERNAL, msg: format!("xz: {}", e) })?;
    let mut enc = liblzma::write::XzEncoder::new_stream(Vec::with_capacity(raw.len() / 3 + 4096), stream);
    enc.write_all(&raw)?;
    Ok((enc.finish()?, raw.len() as u64))
}

fn pack_stream(items: &[&Item], dst: &Path, threads: u32) -> Res<u64> {
    let ps: Vec<(&str, u64)> = items.iter().map(|it| (it.path.as_str(), it.size)).collect();
    let groups = pack_groups(&ps);
    let next = AtomicUsize::new(0);
    let results: Vec<std::sync::Mutex<Option<Res<(Vec<u8>, u64)>>>> = groups.iter().map(|_| std::sync::Mutex::new(None)).collect();
    std::thread::scope(|sc| {
        for _ in 0..threads.max(1) {
            sc.spawn(|| loop {
                let g = next.fetch_add(1, Ordering::SeqCst);
                if g >= groups.len() {
                    break;
                }
                let (a, b) = groups[g];
                *results[g].lock().unwrap() = Some(xz_group(&items[a..b]));
            });
        }
    });
    let f = io_ctx(File::create(dst), || format!("cannot create {}", dst.display()))?;
    let mut w = BufWriter::with_capacity(1 << 22, f);
    let mut total = 0u64;
    for r in results {
        let (bytes, usz) = r.into_inner().unwrap().unwrap_or_else(|| fail(EXIT_INTERNAL, "xz group not done"))?;
        w.write_all(&bytes)?;
        total += usz;
    }
    w.flush()?;
    eprintln!("t2r-slim make: pack in {} independent groups", groups.len());
    Ok(total)
}

pub fn make(o: &MakeOpts) -> Res<()> {
    let t0 = Instant::now();
    let lap = |what: &str| eprintln!("t2r-slim make: {} ({:.0} s)", what, t0.elapsed().as_secs_f64());
    let md = ModData::open(&o.data)?;
    if md.payload_fnv()? != md.fnv {
        return fail(EXIT_IO, "t2r-data.bin: the payload hash in the header is wrong");
    }
    if md.off % 8 != 0 {
        return fail(EXIT_IO, "t2r-data.bin: the payload does not start on an 8-byte boundary");
    }
    let toc = Psarc::open(ModData::open(&o.data)?, "t2r-data.bin")?.toc;
    lap("data file checked");
    let gi = GameIndex::build(&o.game)?;
    lap(&format!("game index: {} paths", gi.by_path.len()));

    let mut staged = Vec::new();
    walk(&o.stage, "", &mut staged)?;
    let mut items: Vec<Item> = Vec::new();
    let mut claimed: HashSet<usize> = HashSet::new();
    let (mut n_side, mut n_shared, mut n_missing, mut n_empty) = (0, 0, 0, 0);
    for (rel, fp) in staged {
        if rel.starts_with("_t2r") {
            continue;
        }
        let e = match toc.get(&path_md5(&rel)) {
            Some(e) => *e,
            None => {
                n_missing += 1;
                continue;
            }
        };
        let size = fs::metadata(&fp)?.len();
        if size != e.size {
            return fail(EXIT_INTERNAL, format!("{}: staged file has {} bytes, the data file {} (stage and data file are from different builds?)", rel, size, e.size));
        }
        if size == 0 {
            n_empty += 1;
            continue;
        }
        let c0 = md.ustart.partition_point(|&s| s < e.off);
        if c0 >= md.ent.len() || md.ustart[c0] != e.off {
            return fail(EXIT_INTERNAL, format!("{} does not start at a chunk", rel));
        }
        let mut chunks = Vec::new();
        let mut c = c0;
        while c < md.ent.len() && md.ustart[c] < e.off + size {
            chunks.push(c);
            c += 1;
        }
        let sum: u64 = chunks.iter().map(|&c| md.ent[c].usz as u64).sum();
        if sum != size || chunks[..chunks.len() - 1].iter().any(|&c| md.ent[c].usz as usize != DSAR_CHUNK) {
            return fail(EXIT_INTERNAL, format!("{}: unexpected chunk layout", rel));
        }
        if !chunks.iter().all(|&c| md.inside(c)) {
            n_side += 1;
            continue;
        }
        if chunks.iter().any(|c| claimed.contains(c)) {
            n_shared += 1;
            continue;
        }
        claimed.extend(chunks.iter().copied());
        let game = gi.pick(&rel, size);
        items.push(Item { path: rel, fp, size, chunks, game });
    }
    let n_game = items.iter().filter(|i| i.game.is_some()).count();
    eprintln!(
        "t2r-slim make: {} files to slim ({} with a game counterpart, {} packed); skipped: {} in the side file, {} sharing data, {} not in the data file, {} empty",
        items.len(),
        n_game,
        items.len() - n_game,
        n_side,
        n_shared,
        n_missing,
        n_empty
    );

    if let Some(c) = &o.cache {
        fs::create_dir_all(c)?;
    }
    let next = AtomicUsize::new(0);
    let order: Vec<usize> = {
        let mut v: Vec<usize> = (0..items.len()).collect();
        v.sort_by_key(|&i| std::cmp::Reverse(items[i].size));
        v
    };
    let results: Vec<(usize, Res<Done>)> = std::thread::scope(|s| {
        let hs: Vec<_> = (0..o.jobs.max(1))
            .map(|_| {
                s.spawn(|| {
                    let mut mine = Vec::new();
                    let mut archives = HashMap::new();
                    loop {
                        let k = next.fetch_add(1, Ordering::Relaxed);
                        if k >= order.len() {
                            break mine;
                        }
                        let i = order[k];
                        let r = work(&md, &items[i], &o.game, o.cache.as_deref(), &mut archives);
                        let failed = r.is_err();
                        mine.push((i, r));
                        if failed {
                            next.store(order.len(), Ordering::Relaxed);
                        }
                    }
                })
            })
            .collect();
        hs.into_iter().flat_map(|h| h.join().unwrap_or_default()).collect()
    });
    let mut done: Vec<Option<Done>> = (0..items.len()).map(|_| None).collect();
    for (i, r) in results {
        done[i] = Some(r?);
    }
    if done.iter().any(|d| d.is_none()) {
        return fail(EXIT_INTERNAL, "a worker stopped early");
    }
    let done: Vec<Done> = done.into_iter().map(|d| d.unwrap()).collect();
    lap("staged files verified against the data file, patches made");

    let mut game_idx: Vec<usize> = (0..items.len()).filter(|&i| items[i].game.is_some()).collect();
    game_idx.sort_by(|&a, &b| items[a].path.cmp(&items[b].path));
    let mut pack_idx: Vec<usize> = (0..items.len()).filter(|&i| items[i].game.is_none()).collect();
    let ext = |p: &str| p.rsplit_once('.').map(|(_, e)| e.to_string()).unwrap_or_default();
    pack_idx.sort_by(|&a, &b| (ext(&items[a].path), &items[a].path).cmp(&(ext(&items[b].path), &items[b].path)));

    let mut tmp_os = o.out.as_os_str().to_owned();
    tmp_os.push(".pack.tmp");
    let pack_tmp = PathBuf::from(tmp_os);
    let pack_items: Vec<&Item> = pack_idx.iter().map(|&i| &items[i]).collect();
    let pack_usz = if pack_items.is_empty() { 0 } else { pack_stream(&pack_items, &pack_tmp, o.xz_threads)? };
    let pack_len = if pack_items.is_empty() { 0 } else { fs::metadata(&pack_tmp)?.len() };
    lap(&format!("pack stream: {:.1} MB -> {:.1} MB", pack_usz as f64 / 1e6, pack_len as f64 / 1e6));

    let mut recs: Vec<Rec> = Vec::new();
    let mut chunks: Vec<Chunk> = Vec::new();
    let mut zeroed: Vec<(u64, u64)> = Vec::new();
    let mut add_chunks = |it: &Item, chunks: &mut Vec<Chunk>| -> (u32, u32) {
        let c0 = chunks.len() as u32;
        for &c in &it.chunks {
            let e = md.ent[c];
            chunks.push(Chunk { payload_off: e.off, usz: e.usz, csz: e.csz });
            zeroed.push((md.off + e.off, e.csz as u64));
        }
        (c0, it.chunks.len() as u32)
    };
    for &i in &game_idx {
        let it = &items[i];
        let (arch, bsz) = it.game.clone().unwrap();
        let (chunk0, nchunks) = add_chunks(it, &mut chunks);
        let dl = done[i].delta.as_ref().map(|d| d.len()).unwrap_or(0) as u64;
        recs.push(Rec { kind: KIND_GAME, base_size: bsz, usz: it.size, delta_off: 0, delta_len: dl, raw_fnv: done[i].raw_fnv, chunk0, nchunks, pack_pos: 0, md5: path_md5(&it.path), arch, path: it.path.clone() });
    }
    let mut pos = 0u64;
    for &i in &pack_idx {
        let it = &items[i];
        let (chunk0, nchunks) = add_chunks(it, &mut chunks);
        recs.push(Rec { kind: KIND_PACK, base_size: 0, usz: it.size, delta_off: 0, delta_len: 0, raw_fnv: done[i].raw_fnv, chunk0, nchunks, pack_pos: pos, md5: path_md5(&it.path), arch: String::new(), path: it.path.clone() });
        pos += it.size;
    }
    let lit_ranges = literal_ranges(&zeroed, md.file_size);
    let lit_bytes: u64 = lit_ranges.iter().map(|r| r.1).sum();

    let tables = HEAD_SIZE as u64 + (recs.len() * REC_SIZE + chunks.len() * CHUNK_SIZE + lit_ranges.len() * LIT_SIZE) as u64;
    let mut at = tables;
    let lits: Vec<Lit> = lit_ranges
        .iter()
        .map(|&(off, len)| {
            let l = Lit { out_off: off, len, dat_off: at };
            at += len;
            l
        })
        .collect();
    let game_recs = game_idx.len();
    for (k, &i) in game_idx.iter().enumerate() {
        let dl = done[i].delta.as_ref().map(|d| d.len()).unwrap_or(0) as u64;
        if dl > 0 {
            recs[k].delta_off = at;
            at += dl;
        }
    }
    let pack_off = at;
    let head = Head {
        blob_off: md.off,
        blob_size: md.size,
        blob_fnv: md.fnv,
        out_size: md.file_size,
        out_fnv: md.file_fnv()?,
        nrec: recs.len() as u32,
        nchunks: chunks.len() as u32,
        nlit: lits.len() as u32,
        pack_off: if pack_len > 0 { pack_off } else { 0 },
        pack_len,
        pack_usz,
        build_id: md.build_id,
    };

    let mut tmp_os = o.out.as_os_str().to_owned();
    tmp_os.push(".tmp");
    let out_tmp = PathBuf::from(tmp_os);
    {
        let mut w = BufWriter::with_capacity(1 << 22, io_ctx(File::create(&out_tmp), || format!("cannot create {}", out_tmp.display()))?);
        w.write_all(&head.to_bytes())?;
        for r in &recs {
            w.write_all(&r.to_bytes()?)?;
        }
        for c in &chunks {
            w.write_all(&c.to_bytes())?;
        }
        for l in &lits {
            w.write_all(&l.to_bytes())?;
        }
        let mut buf = vec![0u8; 1 << 22];
        for l in &lits {
            let mut d = 0u64;
            while d < l.len {
                let n = (l.len - d).min(buf.len() as u64) as usize;
                read_at(&md.f, l.out_off + d, &mut buf[..n])?;
                w.write_all(&buf[..n])?;
                d += n as u64;
            }
        }
        for &i in game_idx.iter() {
            if let Some(d) = &done[i].delta {
                w.write_all(d)?;
            }
        }
        if pack_len > 0 {
            let mut f = File::open(&pack_tmp)?;
            std::io::copy(&mut f, &mut w)?;
        }
        let f = w.into_inner().map_err(|e| Fail { code: EXIT_IO, msg: e.to_string() })?;
        f.sync_all()?;
    }
    let _ = fs::remove_file(&pack_tmp);
    io_ctx(fs::rename(&out_tmp, &o.out), || format!("cannot write {}", o.out.display()))?;
    let total = fs::metadata(&o.out)?.len();
    let patches: u64 = recs[..game_recs].iter().map(|r| r.delta_len).sum();
    let zeroed_bytes: u64 = zeroed.iter().map(|z| z.1).sum();
    eprintln!(
        "t2r-slim make: {} written: {:.1} MB (patches {:.1} MB for {} game files, pack {:.1} MB for {} files, kept bytes {:.1} MB) instead of {:.1} MB; {:.1} MB rebuilt from chunks",
        o.out.display(),
        total as f64 / 1e6,
        patches as f64 / 1e6,
        game_recs,
        pack_len as f64 / 1e6,
        recs.len() - game_recs,
        lit_bytes as f64 / 1e6,
        md.file_size as f64 / 1e6,
        zeroed_bytes as f64 / 1e6
    );
    lap("done");
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn window_log_covers_the_inputs() {
        assert_eq!(window_log(1), 20);
        assert_eq!(window_log(1 << 20), 20);
        assert_eq!(window_log((1 << 20) + 1), 21);
        assert_eq!(window_log(700 << 20), 30);
        assert_eq!(window_log(usize::MAX), 31);
    }

    #[test]
    fn delta_round_trip() {
        let base: Vec<u8> = (0..300_000u32).flat_map(|i| (i.wrapping_mul(2654435761)).to_le_bytes()).collect();
        let mut target = base.clone();
        target[5000..5100].copy_from_slice(&[7u8; 100]);
        target.extend_from_slice(b"tail");
        let d = zstd_delta(&base, &target).unwrap();
        assert!(d.len() < 2000, "{}", d.len());
        let mut dc = zstd_safe::DCtx::create();
        dc.set_parameter(zstd_safe::DParameter::WindowLogMax(31)).unwrap();
        dc.ref_prefix(&base).unwrap();
        let mut out: Vec<u8> = Vec::with_capacity(target.len());
        dc.decompress(&mut out, &d).unwrap();
        assert_eq!(out, target);
    }
}

#[cfg(test)]
mod group_tests {
    use super::*;

    #[test]
    fn groups_cover_all_in_order() {
        let names: Vec<String> = (0..500).map(|i| format!("actor97/f{:04}.pak", i)).collect();
        let ps: Vec<(&str, u64)> = names.iter().enumerate().map(|(i, n)| (n.as_str(), 200_000 + (i as u64 % 7) * 90_000)).collect();
        let g = pack_groups(&ps);
        assert_eq!(g[0].0, 0);
        assert_eq!(g.last().unwrap().1, ps.len());
        for w in g.windows(2) {
            assert_eq!(w[0].1, w[1].0);
        }
        for &(a, b) in &g {
            let sz: u64 = ps[a..b].iter().map(|x| x.1).sum();
            assert!(sz <= GROUP_MAX + 650_000 || b - a == 1);
        }
    }

    #[test]
    fn one_file_change_moves_few_groups() {
        let names: Vec<String> = (0..2000).map(|i| format!("actor97/f{:04}.pak", i)).collect();
        let a: Vec<(&str, u64)> = names.iter().enumerate().map(|(i, n)| (n.as_str(), 150_000 + (i as u64 * 7919 % 13) * 40_000)).collect();
        let mut b = a.clone();
        b[300].1 += 2_000_000;
        let (ga, gb) = (pack_groups(&a), pack_groups(&b));
        let same: usize = gb.iter().filter(|g| ga.contains(g)).count();
        assert!(gb.len() - same <= 3, "{} of {} groups differ", gb.len() - same, gb.len());
    }
}
