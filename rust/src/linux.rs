use crate::excluded::ExcludedDirectories;
use crate::protocol::{DIR, ERROR, FILE, OTHER, Record, UNKNOWN, invalid};
use std::ffi::{CStr, CString};
use std::io;
use std::os::unix::ffi::OsStrExt;
use std::path::Path;
use std::ptr::NonNull;

struct Directory(NonNull<libc::DIR>);

impl Directory {
    fn open(parent: i32, name: &CStr) -> io::Result<Self> {
        let flags = libc::O_RDONLY | libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC;
        let fd = unsafe { libc::openat(parent, name.as_ptr(), flags) };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        let raw = unsafe { libc::fdopendir(fd) };
        match NonNull::new(raw) {
            Some(pointer) => Ok(Self(pointer)),
            None => {
                let err = io::Error::last_os_error();
                unsafe {
                    libc::close(fd);
                }
                Err(err)
            }
        }
    }

    fn fd(&self) -> i32 {
        unsafe { libc::dirfd(self.0.as_ptr()) }
    }

    fn next(&mut self) -> io::Result<Option<&CStr>> {
        // readdir returns null at EOF and on error. The name lives until the
        // next call on this DIR; child directories use separate streams.
        unsafe {
            *libc::__errno_location() = 0;
            let entry = libc::readdir(self.0.as_ptr());
            if entry.is_null() {
                let errno = *libc::__errno_location();
                if errno != 0 {
                    return Err(io::Error::from_raw_os_error(errno));
                }
                return Ok(None);
            }
            Ok(Some(CStr::from_ptr((*entry).d_name.as_ptr())))
        }
    }
}

impl Drop for Directory {
    fn drop(&mut self) {
        unsafe {
            libc::closedir(self.0.as_ptr());
        }
    }
}

fn metadata(fd: i32, name: &CStr) -> io::Result<libc::statx> {
    let mut stat = std::mem::MaybeUninit::uninit();
    let flags = libc::AT_SYMLINK_NOFOLLOW | libc::AT_NO_AUTOMOUNT | libc::AT_EMPTY_PATH;
    let mask = libc::STATX_BASIC_STATS | libc::STATX_MNT_ID;
    if unsafe { libc::statx(fd, name.as_ptr(), flags, mask, stat.as_mut_ptr()) } < 0 {
        return Err(io::Error::last_os_error());
    }
    let stat = unsafe { stat.assume_init() };
    if stat.stx_mask & (libc::STATX_TYPE | libc::STATX_INO | libc::STATX_MNT_ID)
        != (libc::STATX_TYPE | libc::STATX_INO | libc::STATX_MNT_ID)
    {
        return Err(invalid(
            "filesystem did not supply type, inode and mount identity",
        ));
    }
    Ok(stat)
}

fn device(stat: &libc::statx) -> u64 {
    libc::makedev(stat.stx_dev_major, stat.stx_dev_minor) as u64
}

pub fn scan(
    path: &Path,
    recursive: bool,
    mut emit: impl FnMut(Record) -> io::Result<u64>,
) -> io::Result<()> {
    let excluded = ExcludedDirectories::from_env()?;
    let path = CString::new(path.as_os_str().as_bytes())?;
    let root = Directory::open(libc::AT_FDCWD, &path)?;
    let initial = metadata(root.fd(), c"")?;
    let mount = initial.stx_mnt_id;
    let root_id = emit(Record {
        dev: device(&initial),
        ino: initial.stx_ino,
        ..Record::new(DIR, UNKNOWN, &[])
    })?;
    let mut stack = vec![(root, root_id)];
    while let Some((dir, node)) = stack.last_mut() {
        let parent = *node;
        let fd = dir.fd();
        let name = match dir.next() {
            Ok(Some(name)) => name,
            state => {
                if state.is_err() {
                    emit(Record::new(ERROR, parent, &[]))?;
                }
                stack.pop();
                continue;
            }
        };
        let bytes = name.to_bytes();
        if bytes == b"." || bytes == b".." {
            continue;
        }
        let Ok(st) = metadata(fd, name) else {
            emit(Record::new(ERROR, parent, bytes))?;
            continue;
        };
        let kind = match (
            st.stx_mnt_id == mount,
            u32::from(st.stx_mode) & libc::S_IFMT,
        ) {
            (true, libc::S_IFDIR) if !excluded.contains(device(&st), st.stx_ino) => DIR,
            (true, libc::S_IFREG) => FILE,
            _ => OTHER,
        };
        let (logical, allocated) = if kind == FILE {
            if st.stx_mask & libc::STATX_SIZE == 0 {
                emit(Record::new(ERROR, parent, bytes))?;
                continue;
            }
            let alloc = if st.stx_mask & libc::STATX_BLOCKS == 0 {
                UNKNOWN
            } else {
                st.stx_blocks.checked_mul(512).unwrap_or(UNKNOWN)
            };
            (st.stx_size, alloc)
        } else {
            (0, 0)
        };
        let id = emit(Record {
            kind,
            parent,
            logical,
            allocated,
            dev: device(&st),
            ino: st.stx_ino,
            name: bytes,
        })?;
        if kind == DIR && recursive {
            let opened = Directory::open(fd, name).and_then(|child| {
                let actual = metadata(child.fd(), c"")?;
                if actual.stx_mnt_id != mount
                    || actual.stx_ino != st.stx_ino
                    || device(&actual) != device(&st)
                {
                    return Err(invalid("directory changed during scan"));
                }
                Ok(child)
            });
            if let Ok(child) = opened {
                stack.push((child, id));
            } else {
                emit(Record::new(ERROR, id, &[]))?;
            }
        }
    }
    Ok(())
}
