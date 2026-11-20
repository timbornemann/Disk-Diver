use crate::protocol::invalid;
use std::{env, io};

pub struct ExcludedDirectories(Vec<(u64, u64)>);

impl ExcludedDirectories {
    pub fn from_env() -> io::Result<Self> {
        let mut ids = Vec::new();
        if let Some(value) = env::var_os("DISK_DIVER_SKIP_DIR_IDS").filter(|v| !v.is_empty()) {
            let bad = || invalid("invalid excluded directory identity");
            for pair in value.to_str().ok_or_else(bad)?.split(',') {
                let (dev, ino) = pair.split_once(':').ok_or_else(bad)?;
                let number = |s: &str| -> io::Result<u64> {
                    if !s.bytes().all(|c| c.is_ascii_digit()) {
                        return Err(bad());
                    }
                    s.parse().map_err(|_| bad())
                };
                ids.push((number(dev)?, number(ino)?));
            }
        }
        Ok(Self(ids))
    }

    pub fn contains(&self, dev: u64, ino: u64) -> bool {
        ino != 0 && self.0.contains(&(dev, ino))
    }
}
