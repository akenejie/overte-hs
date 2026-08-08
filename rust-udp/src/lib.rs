use std::os::raw::{c_int, c_char};
use std::ffi::{CStr, CString};
use std::net::SocketAddr;
use libc::{sendto, recvfrom, sockaddr, sockaddr_in, sockaddr_in6, sockaddr_storage, socklen_t, AF_INET, AF_INET6, SOCK_DGRAM, socket, close, bind, setsockopt, SOL_SOCKET, SO_REUSEADDR, SO_BROADCAST};
use std::mem;

#[repr(C)]
pub struct RustSocket {
    fd: c_int,
}

#[no_mangle]
pub extern "C" fn rust_socket_create() -> *mut RustSocket {
    let fd = unsafe { socket(AF_INET, SOCK_DGRAM, 0) };
    if fd < 0 {
        return std::ptr::null_mut();
    }
    let boxed = Box::new(RustSocket { fd });
    Box::into_raw(boxed)
}

#[no_mangle]
pub extern "C" fn rust_socket_destroy(sock: *mut RustSocket) {
    if !sock.is_null() {
        unsafe {
            let s = Box::from_raw(sock);
            close(s.fd);
        }
    }
}

#[no_mangle]
pub extern "C" fn rust_socket_set_reuse_addr(sock: *mut RustSocket, reuse: c_int) -> c_int {
    if sock.is_null() { return -1; }
    unsafe {
        let val: c_int = reuse;
        let ret = setsockopt(
            (*sock).fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &val as *const c_int as *const libc::c_void,
            mem::size_of::<c_int>() as socklen_t,
        );
        ret
    }
}

#[no_mangle]
pub extern "C" fn rust_socket_set_broadcast(sock: *mut RustSocket, enable: c_int) -> c_int {
    if sock.is_null() { return -1; }
    unsafe {
        let ret = setsockopt(
            (*sock).fd,
            SOL_SOCKET,
            SO_BROADCAST,
            &enable as *const c_int as *const libc::c_void,
            mem::size_of::<c_int>() as socklen_t,
        );
        ret
    }
}

#[no_mangle]
pub extern "C" fn rust_socket_bind(sock: *mut RustSocket, addr: *const c_char, port: u16) -> c_int {
    if sock.is_null() || addr.is_null() { return -1; }
    unsafe {
        let c_str = CStr::from_ptr(addr);
        let ip_str = match c_str.to_str() {
            Ok(s) => s,
            Err(_) => return -1,
        };
        let addr_str = format!("{}:{}", ip_str, port);
        let socket_addr: SocketAddr = match addr_str.parse() {
            Ok(a) => a,
            Err(_) => return -1,
        };
        match socket_addr {
            SocketAddr::V4(a) => {
                let mut sa: sockaddr_in = mem::zeroed();
                sa.sin_family = AF_INET as u16;
                sa.sin_port = port.to_be();
                sa.sin_addr.s_addr = u32::from_ne_bytes(a.ip().octets()).to_be();
                let ret = bind((*sock).fd, &sa as *const sockaddr_in as *const sockaddr, mem::size_of::<sockaddr_in>() as socklen_t);
                ret
            }
            SocketAddr::V6(a) => {
                let mut sa: sockaddr_in6 = mem::zeroed();
                sa.sin6_family = AF_INET6 as u16;
                sa.sin6_port = port.to_be();
                sa.sin6_addr.s6_addr = a.ip().octets();
                let ret = bind((*sock).fd, &sa as *const sockaddr_in6 as *const sockaddr, mem::size_of::<sockaddr_in6>() as socklen_t);
                ret
            }
        }
    }
}

#[repr(C)]
pub struct RustSockAddrStorage {
    pub addr: [u8; 28],
    pub len: socklen_t,
}

#[no_mangle]
pub extern "C" fn rust_socket_send_to(
    sock: *mut RustSocket,
    data: *const u8,
    data_len: c_int,
    dest_addr: *const c_char,
    dest_port: u16,
) -> c_int {
    if sock.is_null() || data.is_null() || dest_addr.is_null() { return -1; }
    unsafe {
        let c_str = CStr::from_ptr(dest_addr);
        let ip_str = match c_str.to_str() {
            Ok(s) => s,
            Err(_) => return -1,
        };
        let addr_str = format!("{}:{}", ip_str, dest_port);
        let socket_addr: SocketAddr = match addr_str.parse() {
            Ok(a) => a,
            Err(_) => return -1,
        };
        let slice = std::slice::from_raw_parts(data, data_len as usize);
        match socket_addr {
            SocketAddr::V4(a) => {
                let mut sa: sockaddr_in = mem::zeroed();
                sa.sin_family = AF_INET as u16;
                sa.sin_port = dest_port.to_be();
                sa.sin_addr.s_addr = u32::from_ne_bytes(a.ip().octets()).to_be();
                sendto((*sock).fd, slice.as_ptr() as *const libc::c_void, slice.len(), 0, &sa as *const sockaddr_in as *const sockaddr, mem::size_of::<sockaddr_in>() as socklen_t) as c_int
            }
            SocketAddr::V6(a) => {
                let mut sa: sockaddr_in6 = mem::zeroed();
                sa.sin6_family = AF_INET6 as u16;
                sa.sin6_port = dest_port.to_be();
                sa.sin6_addr.s6_addr = a.ip().octets();
                sendto((*sock).fd, slice.as_ptr() as *const libc::c_void, slice.len(), 0, &sa as *const sockaddr_in6 as *const sockaddr, mem::size_of::<sockaddr_in6>() as socklen_t) as c_int
            }
        }
    }
}

#[repr(C)]
pub struct RustRecvResult {
    pub bytes_read: c_int,
    pub src_addr: [u8; 46],
    pub src_port: u16,
}

#[no_mangle]
pub extern "C" fn rust_socket_recv_from(
    sock: *mut RustSocket,
    buf: *mut u8,
    buf_len: c_int,
    result: *mut RustRecvResult,
) -> c_int {
    if sock.is_null() || buf.is_null() || result.is_null() { return -1; }
    unsafe {
        let mut storage: sockaddr_storage = mem::zeroed();
        let mut addrlen: socklen_t = mem::size_of::<sockaddr_storage>() as socklen_t;
        let ret = recvfrom(
            (*sock).fd,
            buf as *mut libc::c_void,
            buf_len as usize,
            0,
            &mut storage as *mut sockaddr_storage as *mut sockaddr,
            &mut addrlen,
        );
        if ret < 0 {
            (*result).bytes_read = -1;
            return -1;
        }
        (*result).bytes_read = ret as c_int;
        let res = &mut *result;
        match storage.ss_family as i32 {
            AF_INET => {
                let sa = &*(&storage as *const sockaddr_storage as *const sockaddr_in);
                let octets = sa.sin_addr.s_addr.to_be().to_ne_bytes();
                let ip_str = format!("{}.{}.{}.{}", octets[0], octets[1], octets[2], octets[3]);
                let c_ip = CString::new(ip_str).unwrap();
                let ip_bytes = c_ip.as_bytes();
                res.src_addr[..ip_bytes.len()].copy_from_slice(ip_bytes);
                res.src_addr[ip_bytes.len()] = 0;
                res.src_port = u16::from_be(sa.sin_port);
            }
            AF_INET6 => {
                let sa = &*(&storage as *const sockaddr_storage as *const sockaddr_in6);
                let octets = sa.sin6_addr.s6_addr;
                let segments: Vec<String> = octets.chunks(2)
                    .map(|c| format!("{:02x}{:02x}", c[0], c[1]))
                    .collect();
                let ip_str = segments.join(":");
                let c_ip = CString::new(ip_str).unwrap();
                let ip_bytes = c_ip.as_bytes();
                res.src_addr[..ip_bytes.len()].copy_from_slice(ip_bytes);
                res.src_addr[ip_bytes.len()] = 0;
                res.src_port = u16::from_be(sa.sin6_port);
            }
            _ => {
                (*result).bytes_read = -1;
                return -1;
            }
        }
        ret as c_int
    }
}

#[no_mangle]
pub extern "C" fn rust_socket_set_nonblocking(sock: *mut RustSocket, nonblocking: c_int) -> c_int {
    if sock.is_null() { return -1; }
    unsafe {
        let flags = libc::fcntl((*sock).fd, libc::F_GETFL, 0);
        if flags < 0 { return -1; }
        let new_flags = if nonblocking != 0 {
            flags | libc::O_NONBLOCK
        } else {
            flags & !libc::O_NONBLOCK
        };
        libc::fcntl((*sock).fd, libc::F_SETFL, new_flags)
    }
}

#[no_mangle]
pub extern "C" fn rust_socket_get_fd(sock: *mut RustSocket) -> c_int {
    if sock.is_null() { return -1; }
    unsafe { (*sock).fd }
}

#[no_mangle]
pub extern "C" fn rust_socket_close(sock: *mut RustSocket) -> c_int {
    if sock.is_null() { return -1; }
    unsafe {
        let s = Box::from_raw(sock);
        close(s.fd)
    }
}
