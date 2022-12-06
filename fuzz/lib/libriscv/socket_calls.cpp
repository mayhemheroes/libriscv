#include <libriscv/machine.hpp>

//#define SOCKETCALL_VERBOSE 1
#ifdef SOCKETCALL_VERBOSE
#define SYSPRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define SYSPRINT(fmt, ...) /* fmt */
#endif

#ifndef WIN32
#include <sys/socket.h>
#else
#include "win32/ws2.hpp"
WSADATA riscv::ws2::global_winsock_data;
bool riscv::ws2::winsock_initialized = false;
#endif

namespace riscv {


template <int W>
static void syscall_socket(Machine<W>& machine)
{
	const auto [domain, type, proto] =
		machine.template sysargs<int, int, int> ();

	SYSPRINT("SYSCALL socket, domain: %x type: %x proto: %x\n",
		domain, type, proto);

	if (machine.has_file_descriptors() && machine.fds().permit_sockets) {
#ifdef WIN32
        ws2::init();
#endif
		auto real_fd = socket(domain, type, proto);
		if (real_fd > 0) {
			const int vfd = machine.fds().assign_socket(real_fd);
			machine.set_result(vfd);
		} else {
			// Translate errno() into kernel API return value
			machine.set_result(-errno);
		}
		return;
	}
	machine.set_result(-EBADF);
}

template <int W>
static void syscall_bind(Machine<W>& machine)
{
	const auto [sockfd, g_addr, addrlen] =
		machine.template sysargs<int, address_type<W>, address_type<W>> ();

	SYSPRINT("SYSCALL bind, sockfd: %d addr: 0x%lX len: 0x%lX\n",
		sockfd, (long)g_addr, (long)addrlen);

	if (addrlen > 0x1000) {
		machine.set_result(-ENOMEM);
		return;
	}

	if (machine.has_file_descriptors() && machine.fds().permit_sockets) {

		const auto real_fd = machine.fds().translate(sockfd);
		alignas(struct sockaddr) char buffer[addrlen];
		machine.copy_from_guest(buffer, g_addr, addrlen);

		int res = bind(real_fd, (struct sockaddr *)buffer, addrlen);
		machine.set_result_or_error(res);
		return;
	}
	machine.set_result(-EBADF);
}

template <int W>
static void syscall_listen(Machine<W>& machine)
{
	const auto [sockfd, backlog] =
		machine.template sysargs<int, int> ();

	SYSPRINT("SYSCALL listen, sockfd: %d backlog: %d\n",
		sockfd, backlog);

	if (machine.has_file_descriptors() && machine.fds().permit_sockets) {

		const auto real_fd = machine.fds().translate(sockfd);

		int res = listen(real_fd, backlog);
		machine.set_result_or_error(res);
		return;
	}
	machine.set_result(-EBADF);
}

template <int W>
static void syscall_accept(Machine<W>& machine)
{
	const auto [sockfd, g_addr, g_addrlen] =
		machine.template sysargs<int, address_type<W>, address_type<W>> ();

	SYSPRINT("SYSCALL accept, sockfd: %d addr: 0x%lX\n",
		sockfd, (long)g_addr);

	if (machine.has_file_descriptors() && machine.fds().permit_sockets) {

		const auto real_fd = machine.fds().translate(sockfd);
		alignas(16) char buffer[128];
		socklen_t addrlen = sizeof(buffer);

		int res = accept(real_fd, (struct sockaddr *)buffer, &addrlen);
		if (res >= 0) {
			// Assign and translate the new fd to virtual fd
			res = machine.fds().assign_socket(res);
			machine.copy_to_guest(g_addr, buffer, addrlen);
			machine.copy_to_guest(g_addrlen, &addrlen, sizeof(addrlen));
		}
		machine.set_result_or_error(res);
		return;
	}
	machine.set_result(-EBADF);
}

template <int W>
static void syscall_connect(Machine<W>& machine)
{
	const auto [sockfd, g_addr, addrlen] =
		machine.template sysargs<int, address_type<W>, address_type<W>> ();

	SYSPRINT("SYSCALL connect, sockfd: %d addr: 0x%lX len: %zu\n",
		sockfd, (long)g_addr, (size_t)addrlen);

	if (addrlen > 256) {
		machine.set_result(-ENOMEM);
		return;
	}

	if (machine.has_file_descriptors() && machine.fds().permit_sockets) {

		const auto real_fd = machine.fds().translate(sockfd);
		alignas(16) char buffer[256];
		machine.copy_from_guest(buffer, g_addr, addrlen);

		const int res = connect(real_fd, (struct sockaddr *)buffer, addrlen);
		printf("connect: %d\n", res);
		machine.set_result_or_error(res);
		return;
	}
	machine.set_result(-EBADF);
}

template <int W>
static void syscall_setsockopt(Machine<W>& machine)
{
	const auto [sockfd, level, optname, g_opt, optlen] =
		machine.template sysargs<int, int, int, address_type<W>, address_type<W>> ();

	SYSPRINT("SYSCALL setsockopt, sockfd: %d level: %x optname: %#x\n",
		sockfd, level, optname);

	if (optlen > 64) {
		machine.set_result(-ENOMEM);
		return;
	}

	if (machine.has_file_descriptors() && machine.fds().permit_sockets) {

		const auto real_fd = machine.fds().translate(sockfd);
		char buffer[optlen];
		machine.copy_from_guest(buffer, g_opt, optlen);

		int res = setsockopt(real_fd, level, optname, buffer, optlen);
		machine.set_result_or_error(res);
		return;
	}

	machine.set_result(-EBADF);
}

template <int W>
void add_socket_syscalls(Machine<W>& machine)
{
	machine.install_syscall_handler(198, syscall_socket<W>);
	machine.install_syscall_handler(200, syscall_bind<W>);
	machine.install_syscall_handler(201, syscall_listen<W>);
	machine.install_syscall_handler(202, syscall_accept<W>);
	machine.install_syscall_handler(203, syscall_connect<W>);
	machine.install_syscall_handler(208, syscall_setsockopt<W>);

}

template void add_socket_syscalls<4>(Machine<4>&);
template void add_socket_syscalls<8>(Machine<8>&);

} // riscv
