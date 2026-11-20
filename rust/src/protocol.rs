use std::io::{self, Read, Write};

pub const MAGIC: &[u8; 4] = b"DDV1";
pub const DIR: u8 = 1;
pub const FILE: u8 = 2;
pub const OTHER: u8 = 3;
pub const ERROR: u8 = 4;
pub const END: u8 = 5;
pub const UNKNOWN: u64 = u64::MAX;

pub fn invalid(reason: &'static str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, reason)
}

#[derive(Clone, Copy)]
pub struct Record<'a> {
    pub kind: u8,
    pub parent: u64,
    pub logical: u64,
    pub allocated: u64,
    pub dev: u64,
    pub ino: u64,
    pub name: &'a [u8],
}

impl<'a> Record<'a> {
    pub fn new(kind: u8, parent: u64, name: &'a [u8]) -> Self {
        Self {
            kind,
            parent,
            name,
            logical: 0,
            allocated: 0,
            dev: 0,
            ino: 0,
        }
    }

    pub fn write(self, out: &mut impl Write) -> io::Result<()> {
        out.write_all(&[self.kind])?;
        for field in [
            self.parent,
            self.logical,
            self.allocated,
            self.dev,
            self.ino,
        ] {
            out.write_all(&field.to_le_bytes())?;
        }
        let len = u32::try_from(self.name.len()).map_err(|_| invalid("name too long"))?;
        out.write_all(&len.to_le_bytes())?;
        out.write_all(self.name)
    }
}

pub fn read(
    mut input: impl Read,
    mut emit: impl FnMut(Record) -> io::Result<u64>,
) -> io::Result<()> {
    let mut magic = [0; 4];
    input.read_exact(&mut magic)?;
    if &magic != MAGIC {
        return Err(invalid("invalid DDV1 signature"));
    }
    let mut name = Vec::new();
    loop {
        let mut kind = [0];
        input.read_exact(&mut kind)?;
        if kind[0] == END {
            if input.read(&mut kind)? != 0 {
                return Err(invalid("data follows DDV1 end"));
            }
            return Ok(());
        }
        if !(DIR..=ERROR).contains(&kind[0]) {
            return Err(invalid("invalid DDV1 record type"));
        }
        let mut header = [0; 44];
        input.read_exact(&mut header)?;
        let num = |start| u64::from_le_bytes(header[start..start + 8].try_into().unwrap());
        let len = u32::from_le_bytes(header[40..].try_into().unwrap()) as usize;
        if len > 65_534 {
            return Err(invalid("name exceeds native component limit"));
        }
        name.resize(len, 0);
        input.read_exact(&mut name)?;
        emit(Record {
            kind: kind[0],
            parent: num(0),
            logical: num(8),
            allocated: num(16),
            dev: num(24),
            ino: num(32),
            name: &name,
        })?;
    }
}
