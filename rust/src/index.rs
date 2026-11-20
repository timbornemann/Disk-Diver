use crate::protocol::{DIR, ERROR, FILE, OTHER, Record, UNKNOWN, invalid};
use std::{collections::HashMap, fmt, io, ops::Range};

struct Node {
    parent: u32,
    name: Range<usize>,
    kind: u8,
    logical: u64,
    allocated: u64,
}

#[derive(Default)]
pub struct Index {
    nodes: Vec<Node>,
    names: Vec<u8>,
    owners: HashMap<(u64, u64), u32>,
    summary: Summary,
}

#[derive(Default)]
pub struct Summary {
    directories: u64,
    files: u64,
    other: u64,
    errors: u64,
    unknown: u64,
    nodes: usize,
    logical: u64,
    allocated: u64,
}

impl fmt::Display for Summary {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{{\"directories\":{},\"files\":{},\"other\":{},\"errors\":{},\"unknown_allocations\":{},\"nodes\":{},\"logical\":{},\"allocated\":{},\"complete\":{}}}",
            self.directories,
            self.files,
            self.other,
            self.errors,
            self.unknown,
            self.nodes,
            self.logical,
            self.allocated,
            self.errors == 0 && self.unknown == 0
        )
    }
}

impl Index {
    pub fn push(&mut self, r: Record<'_>) -> io::Result<u64> {
        let id =
            u32::try_from(self.nodes.len()).map_err(|_| invalid("index exceeds node limit"))?;
        let parent = if id == 0 {
            if r.kind != DIR || r.parent != UNKNOWN || !r.name.is_empty() {
                return Err(invalid("invalid root"));
            }
            u32::MAX
        } else {
            let parent = u32::try_from(r.parent).map_err(|_| invalid("invalid parent"))?;
            let node = self
                .nodes
                .get(parent as usize)
                .ok_or_else(|| invalid("unknown parent"))?;
            if node.kind != DIR && !(r.kind == ERROR && r.name.is_empty()) {
                return Err(invalid("parent must be a directory"));
            }
            if r.name.is_empty() && r.kind != ERROR {
                return Err(invalid("empty component"));
            }
            parent
        };
        let start = self.names.len();
        self.names.extend_from_slice(r.name);
        self.nodes.push(Node {
            parent,
            kind: r.kind,
            name: start..self.names.len(),
            logical: r.logical,
            allocated: r.allocated,
        });
        match r.kind {
            DIR => self.summary.directories += 1,
            OTHER => self.summary.other += 1,
            ERROR => self.summary.errors += 1,
            FILE => {
                self.summary.files += 1;
                if r.ino != 0 {
                    if let Some(&previous) = self.owners.get(&(r.dev, r.ino)) {
                        let discard = if self.path(id) < self.path(previous) {
                            self.owners.insert((r.dev, r.ino), id);
                            previous
                        } else {
                            id
                        };
                        self.nodes[discard as usize].logical = 0;
                        self.nodes[discard as usize].allocated = 0;
                    } else {
                        self.owners.insert((r.dev, r.ino), id);
                    }
                }
            }
            _ => return Err(invalid("unknown node type")),
        }
        Ok(id as u64)
    }

    // Paths are materialized only to resolve a hardlink collision. Windows compares
    // ordinal UTF-16 units. Linux compares the original bytes, including invalid UTF-8.
    fn path(&self, mut i: u32) -> Vec<u16> {
        let mut components = Vec::new();
        while i != 0 {
            let node = &self.nodes[i as usize];
            components.push(&self.names[node.name.clone()]);
            i = node.parent;
        }
        let mut path = Vec::new();
        for part in components.into_iter().rev() {
            path.push(b'/' as u16);
            if cfg!(windows) {
                path.extend(
                    part.chunks_exact(2)
                        .map(|s| u16::from_le_bytes([s[0], s[1]])),
                );
            } else {
                path.extend(part.iter().copied().map(u16::from));
            }
        }
        path
    }

    pub fn finish(mut self) -> io::Result<Summary> {
        if self.nodes.is_empty() {
            return Err(invalid("missing root"));
        }
        for i in (1..self.nodes.len()).rev() {
            let entry = &self.nodes[i];
            let mut allocation = entry.allocated;
            if entry.kind == FILE && allocation == UNKNOWN {
                self.summary.unknown += 1;
                allocation = 0;
            }
            let logical = entry.logical;
            let parent = entry.parent as usize;
            let node = &mut self.nodes[parent];
            node.logical = node
                .logical
                .checked_add(logical)
                .ok_or_else(|| invalid("logical total overflow"))?;
            node.allocated = node
                .allocated
                .checked_add(allocation)
                .ok_or_else(|| invalid("allocated total overflow"))?;
        }
        self.summary.nodes = self.nodes.len();
        self.summary.logical = self.nodes[0].logical;
        self.summary.allocated = self.nodes[0].allocated;
        Ok(self.summary)
    }
}
