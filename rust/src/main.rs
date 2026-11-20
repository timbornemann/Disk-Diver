mod excluded;
mod index;
#[cfg(target_os = "linux")]
#[path = "linux.rs"]
mod platform;
#[cfg(windows)]
#[path = "windows.rs"]
mod platform;
mod protocol;

use index::Index;
use protocol::{Record, invalid};
use std::env;
use std::fs::File;
use std::io::{self, BufReader, BufWriter, Write};
use std::path::Path;

fn run() -> io::Result<()> {
    let args: Vec<_> = env::args_os().skip(1).collect();
    if args.len() != 2 {
        return Err(invalid(
            "usage: disk-diver-rust scan|list|summary <root> or replay <capture>",
        ));
    }
    let path = Path::new(&args[1]);
    match args[0].to_str() {
        Some(command @ ("scan" | "list")) => {
            let mut out = BufWriter::with_capacity(256 * 1024, io::stdout().lock());
            out.write_all(protocol::MAGIC)?;
            let mut next = 0u64;
            platform::scan(path, command == "scan", |record: Record| {
                record.write(&mut out)?;
                let id = next;
                next += 1;
                Ok(id)
            })?;
            out.write_all(&[protocol::END])?;
            out.flush()
        }
        Some(command @ ("summary" | "replay")) => {
            let mut index = Index::default();
            if command == "summary" {
                platform::scan(path, true, |r| index.push(r))?;
            } else {
                protocol::read(
                    BufReader::with_capacity(256 * 1024, File::open(path)?),
                    |r| index.push(r),
                )?;
            }
            writeln!(io::stdout().lock(), "{}", index.finish()?)
        }
        _ => Err(invalid("unknown command")),
    }
}

fn main() -> std::process::ExitCode {
    match run() {
        Ok(()) => std::process::ExitCode::SUCCESS,
        Err(err) => {
            eprintln!("disk-diver-rust: {err}");
            std::process::ExitCode::FAILURE
        }
    }
}
