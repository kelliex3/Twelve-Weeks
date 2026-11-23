mod archive;
mod format;
mod make;
mod moddata;
mod rebuild;
mod util;

use std::path::{Path, PathBuf};

use util::*;

const USAGE: &str = "usage:
  t2r-slim <t2r-data.slim> <game folder> <out t2r-data.bin>
      rebuild t2r-data.bin from the slim file and the game's own files (what the launcher runs)
  t2r-slim make <t2r-data.bin> <stage folder> <game folder> <out t2r-data.slim> [--cache <dir>] [--jobs N] [--xz-threads N]
  t2r-slim extract <t2r-data.bin> <out folder>
      write every packed file of a data file (its own path list) = a stage folder for make when the build's stage is gone
      make the slim file of a build (stage folder = the files the build packed)
  t2r-slim check <t2r-data.bin> <t2r-data.slim> <game folder>
      rebuild into a temporary file next to the slim file and compare it with t2r-data.bin byte for byte";

fn compare(a: &Path, b: &Path) -> Res<()> {
    let fa = io_ctx(std::fs::File::open(a), || format!("cannot open {}", a.display()))?;
    let fb = io_ctx(std::fs::File::open(b), || format!("cannot open {}", b.display()))?;
    let (la, lb) = (fa.metadata()?.len(), fb.metadata()?.len());
    if la != lb {
        return fail(EXIT_INTERNAL, format!("check: sizes differ ({} vs {})", la, lb));
    }
    let mut x = vec![0u8; 1 << 22];
    let mut y = vec![0u8; 1 << 22];
    let mut pos = 0u64;
    while pos < la {
        let n = (la - pos).min(x.len() as u64) as usize;
        read_at(&fa, pos, &mut x[..n])?;
        read_at(&fb, pos, &mut y[..n])?;
        if x[..n] != y[..n] {
            let k = x[..n].iter().zip(&y[..n]).position(|(p, q)| p != q).unwrap_or(0);
            return fail(EXIT_INTERNAL, format!("check: the rebuilt file differs at byte {}", pos + k as u64));
        }
        pos += n as u64;
    }
    Ok(())
}

fn run(args: &[String]) -> Res<()> {
    match args.first().map(String::as_str) {
        Some("make") => {
            let mut pos: Vec<&String> = Vec::new();
            let (mut cache, mut jobs, mut xz) = (None, 4usize, 4u32);
            let mut i = 1;
            while i < args.len() {
                let v = args.get(i + 1);
                match args[i].as_str() {
                    "--cache" => cache = v.map(PathBuf::from),
                    "--jobs" => jobs = v.and_then(|v| v.parse().ok()).unwrap_or(0),
                    "--xz-threads" => xz = v.and_then(|v| v.parse().ok()).unwrap_or(0),
                    _ => {
                        pos.push(&args[i]);
                        i += 1;
                        continue;
                    }
                }
                if v.is_none() || jobs == 0 || xz == 0 {
                    return fail(EXIT_IO, format!("{} needs a value\n{}", args[i], USAGE));
                }
                i += 2;
            }
            if pos.len() != 4 {
                return fail(EXIT_IO, USAGE);
            }
            make::make(&make::MakeOpts {
                data: pos[0].into(),
                stage: pos[1].into(),
                game: pos[2].into(),
                out: pos[3].into(),
                cache,
                jobs,
                xz_threads: xz,
            })
        }
        Some("extract") if args.len() == 3 => {
            let mut ps = archive::Psarc::open(moddata::ModData::open(Path::new(&args[1]))?, "t2r-data.bin")?;
            let out = Path::new(&args[2]);
            let mut n = 0usize; let mut skipped = 0usize;
            for p in ps.paths("t2r-data.bin")? {
                let rel = p.trim_start_matches('/');
                if rel.is_empty() || rel.contains("..") { continue; }
                let got = match ps.file(&archive::path_md5(rel), "t2r-data.bin") { Ok(v) => v, Err(_) => { skipped += 1; continue; } };   // data in part1.dat: not in this file
                if let Some(b) = got {
                    let fp = out.join(rel);
                    if let Some(d) = fp.parent() { std::fs::create_dir_all(d).map_err(|e| Fail { code: EXIT_IO, msg: e.to_string() })?; }
                    std::fs::write(&fp, &b).map_err(|e| Fail { code: EXIT_IO, msg: e.to_string() })?;
                    n += 1;
                } else { eprintln!("extract: no toc entry for {}", rel); }
            }
            println!("extract: {} files, {} skipped (side file)", n, skipped);
            Ok(())
        }
        Some("check") if args.len() == 4 => {
            let data = Path::new(&args[1]);
            let slim = Path::new(&args[2]);
            let mut out = slim.as_os_str().to_owned();
            out.push(".check.bin");
            let out = PathBuf::from(out);
            rebuild::rebuild(slim, Path::new(&args[3]), &out)?;
            let r = compare(data, &out);
            let _ = std::fs::remove_file(&out);
            r?;
            println!("t2r-slim check: the rebuilt file is byte-identical to {}", data.display());
            Ok(())
        }
        Some(_) if args.len() == 3 => rebuild::rebuild(Path::new(&args[0]), Path::new(&args[1]), Path::new(&args[2])),
        _ => fail(EXIT_IO, USAGE),
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if let Err(e) = run(&args) {
        eprintln!("t2r-slim: {}", e.msg);
        std::process::exit(e.code);
    }
}
