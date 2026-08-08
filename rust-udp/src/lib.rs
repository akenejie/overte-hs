use std::os::raw::{c_int, c_char};
use std::ffi::{CStr, CString};
use std::net::SocketAddr;
use std::mem;

#[repr(C)]
pub struct RustSocket {
    fd: c_int,
}

#[repr(C)]
pub struct RustSockAddrStorage {
    pub addr: [u8; 28],
    pub len: u32,
}

#[repr(C)]
pub struct RustRecvResult {
    pub bytes_read: c_int,
    pub src_addr: [u8; 46],
    pub src_port: u16,
}

fn to_socketaddr(ip: &str, port: u16) -> Option<SocketAddr> {
    format!("{}:{}", ip, port).parse().ok()
}

fn write_ip_str(res: &mut RustRecvResult, ip: &str, port: u16) {
    let c_ip = CString::new(ip).unwrap();
    let ip_bytes = c_ip.as_bytes();
    res.src_addr[..ip_bytes.len()].copy_from_slice(ip_bytes);
    res.src_addr[ip_bytes.len()] = 0;
    res.src_port = port;
}

#[cfg(any(target_os = "linux", target_os = "macos"))]
mod imp {
    use super::*;
use libc::{
        sockaddr, sockaddr_in, sockaddr_in6, sockaddr_storage, socklen_t,
        SO_REUSEADDR, SO_BROADCAST,
        sendto, recvfrom, socket, setsockopt,
        fcntl, F_GETFL, F_SETFL, O_NONBLOCK,
    };

    pub fn create() -> *mut RustSocket {
        let fd = unsafe { socket(libc::AF_INET, libc::SOCK_DGRAM, 0) };
        if fd < 0 {
            return std::ptr::null_mut();
        }
        let boxed = Box::new(RustSocket { fd });
        Box::into_raw(boxed)
    }

    pub fn destroy(sock: *mut RustSocket) {
        if !sock.is_null() {
            unsafe {
                let s = Box::from_raw(sock);
                libc::close(s.fd);
            }
        }
    }

    pub fn set_reuse_addr(sock: *mut RustSocket, reuse: c_int) -> c_int {
        if sock.is_null() { return -1; }
        unsafe {
            let val: c_int = reuse;
            setsockopt(
                (*sock).fd,
                libc::SOL_SOCKET,
                SO_REUSEADDR,
                &val as *const c_int as *const libc::c_void,
                mem::size_of::<c_int>() as socklen_t,
            )
        }
    }

    pub fn set_broadcast(sock: *mut RustSocket, enable: c_int) -> c_int {
        if sock.is_null() { return -1; }
        unsafe {
            let val: c_int = enable;
            setsockopt(
                (*sock).fd,
                libc::SOL_SOCKET,
                SO_BROADCAST,
                &val as *const c_int as *const libc::c_void,
                mem::size_of::<c_int>() as socklen_t,
            )
        }
    }

    fn build_v4_sockaddr(a: &std::net::SocketAddrV4, port: u16) -> sockaddr_in {
        let mut sa: sockaddr_in = unsafe { mem::zeroed() };
        sa.sin_family = libc::AF_INET as libc::sa_family_t;
        sa.sin_port = port.to_be();
        sa.sin_addr.s_addr = u32::from_ne_bytes(a.ip().octets()).to_be();
        sa
    }

    fn build_v6_sockaddr(a: &std::net::SocketAddrV6, port: u16) -> sockaddr_in6 {
        let mut sa: sockaddr_in6 = unsafe { mem::zeroed() };
        sa.sin6_family = libc::AF_INET6 as libc::sa_family_t;
        sa.sin6_port = port.to_be();
        sa.sin6_addr.s6_addr = a.ip().octets();
        sa
    }

    pub fn bind_socket(sock: *mut RustSocket, addr: *const c_char, port: u16) -> c_int {
        if sock.is_null() || addr.is_null() { return -1; }
        unsafe {
            let c_str = CStr::from_ptr(addr);
            let ip_str = match c_str.to_str() { Ok(s) => s, Err(_) => return -1 };
            let socket_addr = match to_socketaddr(ip_str, port) { Some(a) => a, None => return -1 };
            match socket_addr {
                SocketAddr::V4(a) => {
                    let sa = build_v4_sockaddr(&a, port);
                    libc::bind((*sock).fd, &sa as *const sockaddr_in as *const sockaddr, mem::size_of::<sockaddr_in>() as socklen_t)
                }
                SocketAddr::V6(a) => {
                    let sa = build_v6_sockaddr(&a, port);
                    libc::bind((*sock).fd, &sa as *const sockaddr_in6 as *const sockaddr, mem::size_of::<sockaddr_in6>() as socklen_t)
                }
            }
        }
    }

    pub fn send_to(
        sock: *mut RustSocket,
        data: *const u8,
        data_len: c_int,
        dest_addr: *const c_char,
        dest_port: u16,
    ) -> c_int {
        if sock.is_null() || data.is_null() || dest_addr.is_null() { return -1; }
        unsafe {
            let c_str = CStr::from_ptr(dest_addr);
            let ip_str = match c_str.to_str() { Ok(s) => s, Err(_) => return -1 };
            let socket_addr = match to_socketaddr(ip_str, dest_port) { Some(a) => a, None => return -1 };
            let slice = std::slice::from_raw_parts(data, data_len as usize);
            match socket_addr {
                SocketAddr::V4(a) => {
                    let sa = build_v4_sockaddr(&a, dest_port);
                    sendto((*sock).fd, slice.as_ptr() as *const libc::c_void, slice.len(), 0, &sa as *const sockaddr_in as *const sockaddr, mem::size_of::<sockaddr_in>() as socklen_t) as c_int
                }
                SocketAddr::V6(a) => {
                    let sa = build_v6_sockaddr(&a, dest_port);
                    sendto((*sock).fd, slice.as_ptr() as *const libc::c_void, slice.len(), 0, &sa as *const sockaddr_in6 as *const sockaddr, mem::size_of::<sockaddr_in6>() as socklen_t) as c_int
                }
            }
        }
    }

    pub fn recv_from(
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
                libc::AF_INET => {
                    let sa = &*(&storage as *const sockaddr_storage as *const sockaddr_in);
                    let octets = sa.sin_addr.s_addr.to_be().to_ne_bytes();
                    let ip_str = format!("{}.{}.{}.{}", octets[0], octets[1], octets[2], octets[3]);
                    write_ip_str(res, &ip_str, u16::from_be(sa.sin_port));
                }
                libc::AF_INET6 => {
                    let sa = &*(&storage as *const sockaddr_storage as *const sockaddr_in6);
                    let octets = sa.sin6_addr.s6_addr;
                    let segments: Vec<String> = octets.chunks(2)
                        .map(|c| format!("{:02x}{:02x}", c[0], c[1]))
                        .collect();
                    let ip_str = segments.join(":");
                    write_ip_str(res, &ip_str, u16::from_be(sa.sin6_port));
                }
                _ => {
                    (*result).bytes_read = -1;
                    return -1;
                }
            }
            ret as c_int
        }
    }

    pub fn set_nonblocking(sock: *mut RustSocket, nonblocking: c_int) -> c_int {
        if sock.is_null() { return -1; }
        unsafe {
            let flags = fcntl((*sock).fd, F_GETFL, 0);
            if flags < 0 { return -1; }
            let new_flags = if nonblocking != 0 {
                flags | O_NONBLOCK
            } else {
                flags & !O_NONBLOCK
            };
            fcntl((*sock).fd, F_SETFL, new_flags)
        }
    }

    pub fn get_fd(sock: *mut RustSocket) -> c_int {
        if sock.is_null() { return -1; }
        unsafe { (*sock).fd }
    }

    pub fn close(sock: *mut RustSocket) -> c_int {
        if sock.is_null() { return -1; }
        unsafe {
            let s = Box::from_raw(sock);
            libc::close(s.fd)
        }
    }
}

#[cfg(target_os = "windows")]
mod imp {
    use super::*;

    const AF_INET: c_int = 2;
    const AF_INET6: c_int = 23;
    const SOCK_DGRAM: c_int = 2;
    const SOL_SOCKET: c_int = 0xffff;
    const SO_REUSEADDR: c_int = 0x04;
    const SO_BROADCAST: c_int = 0x20;
    const FIONBIO: u32 = 0x8004667e;

    #[repr(C)]
    #[derive(Clone, Copy)]
    struct in_addr {
        s_addr: u32,
    }

    #[repr(C)]
    #[derive(Clone, Copy)]
    struct in6_addr {
        s6_addr: [u8; 16],
    }

    #[repr(C)]
    struct sockaddr_in {
        sin_family: u16,
        sin_port: u16,
        sin_addr: in_addr,
        sin_zero: [u8; 8],
    }

    #[repr(C)]
    struct sockaddr_in6 {
        sin6_family: u16,
        sin6_port: u16,
        sin6_flowinfo: u32,
        sin6_addr: in6_addr,
        sin6_scope_id: u32,
    }

    #[link(name = "ws2_32")]
    unsafe extern "system" {
        fn WSASocketW(
            af: c_int,
            sock_type: c_int,
            protocol: c_int,
            info: *const u8,
            group: u32,
            flags: u32,
        ) -> usize;
        fn bind(s: usize, name: *const u8, namelen: c_int) -> c_int;
        fn sendto(
            s: usize,
            buf: *const u8,
            len: c_int,
            flags: c_int,
            to: *const u8,
            tolen: c_int,
        ) -> c_int;
        fn recvfrom(
            s: usize,
            buf: *mut u8,
            len: c_int,
            flags: c_int,
            from: *mut u8,
            fromlen: *mut c_int,
        ) -> c_int;
        fn setsockopt(
            s: usize,
            level: c_int,
            optname: c_int,
            optval: *const u8,
            optlen: c_int,
        ) -> c_int;
        fn closesocket(s: usize) -> c_int;
        fn ioctlsocket(s: usize, cmd: u32, argp: *mut u32) -> c_int;
    }

    fn sockfd(sock: *mut RustSocket) -> usize {
        unsafe { (*sock).fd as usize }
    }

    pub fn create() -> *mut RustSocket {
        let fd = unsafe { WSASocketW(AF_INET, SOCK_DGRAM, 0, std::ptr::null(), 0, 0) };
        if (fd as isize) == !0 {
            return std::ptr::null_mut();
        }
        let boxed = Box::new(RustSocket { fd: fd as c_int });
        Box::into_raw(boxed)
    }

    pub fn destroy(sock: *mut RustSocket) {
        if !sock.is_null() {
            unsafe {
                let s = Box::from_raw(sock);
                closesocket(s.fd as usize);
            }
        }
    }

    pub fn set_reuse_addr(sock: *mut RustSocket, reuse: c_int) -> c_int {
        if sock.is_null() { return -1; }
        unsafe {
            setsockopt(sockfd(sock), SOL_SOCKET, SO_REUSEADDR, &reuse as *const c_int as *const u8, mem::size_of::<c_int>() as c_int)
        }
    }

    pub fn set_broadcast(sock: *mut RustSocket, enable: c_int) -> c_int {
        if sock.is_null() { return -1; }
        unsafe {
            setsockopt(sockfd(sock), SOL_SOCKET, SO_BROADCAST, &enable as *const c_int as *const u8, mem::size_of::<c_int>() as c_int)
        }
    }

    fn build_v4_sockaddr(a: &std::net::SocketAddrV4, port: u16) -> sockaddr_in {
        let mut sa: sockaddr_in = unsafe { mem::zeroed() };
        sa.sin_family = AF_INET as u16;
        sa.sin_port = port.to_be();
        sa.sin_addr.s_addr = u32::from_ne_bytes(a.ip().octets()).to_be();
        sa
    }

    fn build_v6_sockaddr(a: &std::net::SocketAddrV6, port: u16) -> sockaddr_in6 {
        let mut sa: sockaddr_in6 = unsafe { mem::zeroed() };
        sa.sin6_family = AF_INET6 as u16;
        sa.sin6_port = port.to_be();
        sa.sin6_addr.s6_addr = a.ip().octets();
        sa.sin6_scope_id = a.scope_id();
        sa
    }

    pub fn bind_socket(sock: *mut RustSocket, addr: *const c_char, port: u16) -> c_int {
        if sock.is_null() || addr.is_null() { return -1; }
        unsafe {
            let c_str = CStr::from_ptr(addr);
            let ip_str = match c_str.to_str() { Ok(s) => s, Err(_) => return -1 };
            let socket_addr = match to_socketaddr(ip_str, port) { Some(a) => a, None => return -1 };
            match socket_addr {
                SocketAddr::V4(a) => {
                    let sa = build_v4_sockaddr(&a, port);
                    bind(sockfd(sock), &sa as *const sockaddr_in as *const u8, mem::size_of::<sockaddr_in>() as c_int)
                }
                SocketAddr::V6(a) => {
                    let sa = build_v6_sockaddr(&a, port);
                    bind(sockfd(sock), &sa as *const sockaddr_in6 as *const u8, mem::size_of::<sockaddr_in6>() as c_int)
                }
            }
        }
    }

    pub fn send_to(
        sock: *mut RustSocket,
        data: *const u8,
        data_len: c_int,
        dest_addr: *const c_char,
        dest_port: u16,
    ) -> c_int {
        if sock.is_null() || data.is_null() || dest_addr.is_null() { return -1; }
        unsafe {
            let c_str = CStr::from_ptr(dest_addr);
            let ip_str = match c_str.to_str() { Ok(s) => s, Err(_) => return -1 };
            let socket_addr = match to_socketaddr(ip_str, dest_port) { Some(a) => a, None => return -1 };
            let slice = std::slice::from_raw_parts(data, data_len as usize);
            match socket_addr {
                SocketAddr::V4(a) => {
                    let sa = build_v4_sockaddr(&a, dest_port);
                    sendto(sockfd(sock), slice.as_ptr(), slice.len() as c_int, 0, &sa as *const sockaddr_in as *const u8, mem::size_of::<sockaddr_in>() as c_int)
                }
                SocketAddr::V6(a) => {
                    let sa = build_v6_sockaddr(&a, dest_port);
                    sendto(sockfd(sock), slice.as_ptr(), slice.len() as c_int, 0, &sa as *const sockaddr_in6 as *const u8, mem::size_of::<sockaddr_in6>() as c_int)
                }
            }
        }
    }

    pub fn recv_from(
        sock: *mut RustSocket,
        buf: *mut u8,
        buf_len: c_int,
        result: *mut RustRecvResult,
    ) -> c_int {
        if sock.is_null() || buf.is_null() || result.is_null() { return -1; }
        unsafe {
            let mut storage: [u8; 28] = mem::zeroed();
            let mut addrlen: c_int = 28;
            let ret = recvfrom(
                sockfd(sock),
                buf,
                buf_len,
                0,
                storage.as_mut_ptr(),
                &mut addrlen,
            );
            if ret < 0 {
                (*result).bytes_read = -1;
                return -1;
            }
            (*result).bytes_read = ret as c_int;
            let res = &mut *result;
            let family = u16::from_ne_bytes([storage[0], storage[1]]);
            match family {
                2 => {
                    let sa = &*(storage.as_ptr() as *const sockaddr_in);
                    let octets = sa.sin_addr.s_addr.to_be().to_ne_bytes();
                    let ip_str = format!("{}.{}.{}.{}", octets[0], octets[1], octets[2], octets[3]);
                    write_ip_str(res, &ip_str, u16::from_be(sa.sin_port));
                }
                23 => {
                    let sa = &*(storage.as_ptr() as *const sockaddr_in6);
                    let octets = sa.sin6_addr.s6_addr;
                    let segments: Vec<String> = octets.chunks(2)
                        .map(|c| format!("{:02x}{:02x}", c[0], c[1]))
                        .collect();
                    let ip_str = segments.join(":");
                    write_ip_str(res, &ip_str, u16::from_be(sa.sin6_port));
                }
                _ => {
                    (*result).bytes_read = -1;
                    return -1;
                }
            }
            ret as c_int
        }
    }

    pub fn set_nonblocking(sock: *mut RustSocket, nonblocking: c_int) -> c_int {
        if sock.is_null() { return -1; }
        unsafe {
            let mut v: u32 = if nonblocking != 0 { 1 } else { 0 };
            ioctlsocket(sockfd(sock), FIONBIO, &mut v)
        }
    }

    pub fn get_fd(sock: *mut RustSocket) -> c_int {
        if sock.is_null() { return -1; }
        unsafe { (*sock).fd }
    }

    pub fn close(sock: *mut RustSocket) -> c_int {
        if sock.is_null() { return -1; }
        unsafe {
            let s = Box::from_raw(sock);
            closesocket(s.fd as usize)
        }
    }
}

#[no_mangle]
pub extern "C" fn rust_socket_create() -> *mut RustSocket {
    imp::create()
}

#[no_mangle]
pub extern "C" fn rust_socket_destroy(sock: *mut RustSocket) {
    imp::destroy(sock)
}

#[no_mangle]
pub extern "C" fn rust_socket_set_reuse_addr(sock: *mut RustSocket, reuse: c_int) -> c_int {
    imp::set_reuse_addr(sock, reuse)
}

#[no_mangle]
pub extern "C" fn rust_socket_set_broadcast(sock: *mut RustSocket, enable: c_int) -> c_int {
    imp::set_broadcast(sock, enable)
}

#[no_mangle]
pub extern "C" fn rust_socket_bind(sock: *mut RustSocket, addr: *const c_char, port: u16) -> c_int {
    imp::bind_socket(sock, addr, port)
}

#[no_mangle]
pub extern "C" fn rust_socket_send_to(
    sock: *mut RustSocket,
    data: *const u8,
    data_len: c_int,
    dest_addr: *const c_char,
    dest_port: u16,
) -> c_int {
    imp::send_to(sock, data, data_len, dest_addr, dest_port)
}

#[no_mangle]
pub extern "C" fn rust_socket_recv_from(
    sock: *mut RustSocket,
    buf: *mut u8,
    buf_len: c_int,
    result: *mut RustRecvResult,
) -> c_int {
    imp::recv_from(sock, buf, buf_len, result)
}

#[no_mangle]
pub extern "C" fn rust_socket_set_nonblocking(sock: *mut RustSocket, nonblocking: c_int) -> c_int {
    imp::set_nonblocking(sock, nonblocking)
}

#[no_mangle]
pub extern "C" fn rust_socket_get_fd(sock: *mut RustSocket) -> c_int {
    imp::get_fd(sock)
}

#[no_mangle]
pub extern "C" fn rust_socket_close(sock: *mut RustSocket) -> c_int {
    imp::close(sock)
}