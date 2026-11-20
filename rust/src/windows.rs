use crate::excluded::ExcludedDirectories;
use crate::protocol::{DIR, ERROR, FILE, OTHER, Record, UNKNOWN, invalid};
use std::fs::{File, OpenOptions};
use std::io;
use std::mem::{offset_of, size_of, size_of_val};
use std::os::windows::{
    fs::OpenOptionsExt,
    io::{AsRawHandle, FromRawHandle},
};
use std::path::Path;
use windows_sys::Wdk::{
    Foundation::OBJECT_ATTRIBUTES,
    Storage::FileSystem::{
        FILE_DIRECTORY_FILE, FILE_OPEN_FOR_BACKUP_INTENT, FILE_OPEN_REPARSE_POINT,
        FILE_SYNCHRONOUS_IO_NONALERT, NtOpenFile,
    },
};
use windows_sys::Win32::{
    Foundation::{ERROR_NO_MORE_FILES, RtlNtStatusToDosError, UNICODE_STRING},
    Storage::FileSystem::*,
    System::IO::IO_STATUS_BLOCK,
};

const SHARE: u32 = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
const ACCESS: u32 = FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE;

struct Child {
    id: u64,
    ino: u64,
    name: Vec<u16>,
}
struct Frame {
    handle: File,
    children: Vec<Child>,
}

fn info(file: &File) -> io::Result<BY_HANDLE_FILE_INFORMATION> {
    let mut data = BY_HANDLE_FILE_INFORMATION::default();
    if unsafe { GetFileInformationByHandle(file.as_raw_handle(), &mut data) } == 0 {
        return Err(io::Error::last_os_error());
    }
    if data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT != 0
        || data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY == 0
    {
        return Err(invalid("expected a directory without a reparse point"));
    }
    Ok(data)
}

fn inode(data: &BY_HANDLE_FILE_INFORMATION) -> u64 {
    (u64::from(data.nFileIndexHigh) << 32) | u64::from(data.nFileIndexLow)
}

// Open this one component relative to its still-open parent. A rename or a
// newly substituted junction cannot redirect the scanner through another tree.
fn open_child(parent: &File, name: &mut [u16]) -> io::Result<File> {
    let bytes = u16::try_from(size_of_val(name)).map_err(|_| invalid("directory name too long"))?;
    let name = UNICODE_STRING {
        Length: bytes,
        MaximumLength: bytes,
        Buffer: name.as_mut_ptr(),
    };
    let attributes = OBJECT_ATTRIBUTES {
        Length: size_of::<OBJECT_ATTRIBUTES>() as u32,
        RootDirectory: parent.as_raw_handle(),
        ObjectName: &name,
        ..Default::default()
    };
    let mut handle = std::ptr::null_mut();
    let mut iosb = IO_STATUS_BLOCK::default();
    let options = FILE_DIRECTORY_FILE
        | FILE_OPEN_FOR_BACKUP_INTENT
        | FILE_OPEN_REPARSE_POINT
        | FILE_SYNCHRONOUS_IO_NONALERT;
    let status = unsafe { NtOpenFile(&mut handle, ACCESS, &attributes, &mut iosb, SHARE, options) };
    if status < 0 {
        return Err(io::Error::from_raw_os_error(
            unsafe { RtlNtStatusToDosError(status) } as i32,
        ));
    }
    Ok(unsafe { File::from_raw_handle(handle) })
}

fn entries(
    handle: File,
    parent: u64,
    dev: u64,
    recursive: bool,
    excluded: &ExcludedDirectories,
    buffer: &mut [u64],
    emit: &mut impl FnMut(Record) -> io::Result<u64>,
) -> io::Result<Frame> {
    let mut children = Vec::new();
    let mut class = FileIdBothDirectoryRestartInfo;
    let len = size_of_val(buffer);
    loop {
        let ok = unsafe {
            GetFileInformationByHandleEx(
                handle.as_raw_handle(),
                class,
                buffer.as_mut_ptr().cast(),
                len as u32,
            )
        };
        if ok == 0 {
            if io::Error::last_os_error().raw_os_error() != Some(ERROR_NO_MORE_FILES as i32) {
                emit(Record::new(ERROR, parent, &[]))?;
            }
            break;
        }
        class = FileIdBothDirectoryInfo;
        // Win32 returns the records as an aligned chain with variable-size tails.
        // Read the integer fields from byte slices to avoid unaligned references.
        let bytes = unsafe { std::slice::from_raw_parts(buffer.as_ptr().cast::<u8>(), len) };
        let mut offset = 0;
        const START: usize = offset_of!(FILE_ID_BOTH_DIR_INFO, FileName);
        loop {
            let entry = &bytes[offset..];
            if entry.len() < START {
                return Err(invalid("truncated native directory record"));
            }
            let long = |i| u32::from_le_bytes(entry[i..i + 4].try_into().unwrap());
            let wide = |i| u64::from_le_bytes(entry[i..i + 8].try_into().unwrap());
            let next = long(offset_of!(FILE_ID_BOTH_DIR_INFO, NextEntryOffset)) as usize;
            let limit = if next == 0 { entry.len() } else { next };
            let size = long(offset_of!(FILE_ID_BOTH_DIR_INFO, FileNameLength)) as usize;
            if limit > entry.len() || !size.is_multiple_of(2) || START + size > limit {
                return Err(invalid("invalid native directory record"));
            }
            let name = &entry[START..START + size];
            if name != b".\0" && name != b".\0.\0" {
                let flags = long(offset_of!(FILE_ID_BOTH_DIR_INFO, FileAttributes));
                let ino = wide(offset_of!(FILE_ID_BOTH_DIR_INFO, FileId));
                let kind = match flags {
                    f if f & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE) != 0 => OTHER,
                    f if f & FILE_ATTRIBUTE_DIRECTORY == 0 => FILE,
                    _ if excluded.contains(dev, ino) => OTHER,
                    _ => DIR,
                };
                let (logical, allocated) = if kind == FILE {
                    (
                        wide(offset_of!(FILE_ID_BOTH_DIR_INFO, EndOfFile)),
                        wide(offset_of!(FILE_ID_BOTH_DIR_INFO, AllocationSize)),
                    )
                } else {
                    (0, 0)
                };
                let id = emit(Record {
                    kind,
                    parent,
                    logical,
                    allocated,
                    dev,
                    ino,
                    name,
                })?;
                if kind == DIR && recursive {
                    children.push(Child {
                        id,
                        ino,
                        name: name
                            .chunks_exact(2)
                            .map(|s| u16::from_le_bytes([s[0], s[1]]))
                            .collect(),
                    });
                }
            }
            if next == 0 {
                break;
            }
            offset += next;
        }
    }
    Ok(Frame { handle, children })
}

pub fn scan(
    path: &Path,
    recursive: bool,
    mut emit: impl FnMut(Record) -> io::Result<u64>,
) -> io::Result<()> {
    let excluded = ExcludedDirectories::from_env()?;
    let handle = OpenOptions::new()
        .access_mode(ACCESS)
        .share_mode(SHARE)
        .custom_flags(FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT)
        .open(path)?;
    let root = info(&handle)?;
    let dev = u64::from(root.dwVolumeSerialNumber);
    let id = emit(Record {
        dev,
        ino: inode(&root),
        ..Record::new(DIR, UNKNOWN, &[])
    })?;
    let mut buffer = vec![0u64; 8192];
    let mut stack = vec![entries(
        handle,
        id,
        dev,
        recursive,
        &excluded,
        &mut buffer,
        &mut emit,
    )?];
    while let Some(frame) = stack.last_mut() {
        let Some(mut child) = frame.children.pop() else {
            stack.pop();
            continue;
        };
        let opened = open_child(&frame.handle, &mut child.name).and_then(|handle| {
            let actual = info(&handle)?;
            if u64::from(actual.dwVolumeSerialNumber) != dev
                || (child.ino != 0 && inode(&actual) != child.ino)
            {
                return Err(invalid("directory changed during scan"));
            }
            Ok(handle)
        });
        if let Ok(handle) = opened {
            stack.push(entries(
                handle,
                child.id,
                dev,
                true,
                &excluded,
                &mut buffer,
                &mut emit,
            )?);
        } else {
            emit(Record::new(ERROR, child.id, &[]))?;
        }
    }
    Ok(())
}
