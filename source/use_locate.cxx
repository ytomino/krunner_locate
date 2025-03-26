#include "use_locate.hxx"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <linux/limits.h>
#include <malloc.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int do_close(int fd)
{
	while(close(fd) < 0){
		int error;
		if((error = errno) != EINTR) return error;
	}
	return 0;
}

static int do_waitpid(int pid, int *status, int options)
{
	while(waitpid(pid, status, options) < 0){
		int error;
		if((error = errno) != EINTR) return error;
	}
	return 0;
}

static int set_cloexec(int fd)
{
	int flags = fcntl(fd, F_GETFD, 0);
	if(flags < 0) return errno;
	if(fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) return errno;
	return 0;
}

static char const locate_path[] = "/usr/bin/locate";

static int spawn_locate(
	std::string_view pattern, bool base_name, bool ignore_case, int outfd, int *pid
)
{
	int error;
	
	/* argv */
	char *c_pattern = strndup(pattern.data(), pattern.size());
		/* strndup adds '\0' */
	char const *argv[9];
	int argc = 0;
	argv[argc ++] = locate_path;
	argv[argc ++] = "-0";
	if(base_name){
		argv[argc ++] = "-b";
	}
	if(ignore_case){
		argv[argc ++] = "-i";
	}
	argv[argc ++] = "-l";
	argv[argc ++] = "1024";
	argv[argc ++] = "--";
	argv[argc ++] = c_pattern;
	argv[argc] = nullptr;
	assert(argc < 9);
	
	/* file_actions */
	posix_spawn_file_actions_t file_actions;
	if((error = posix_spawn_file_actions_init(&file_actions)) != 0){
		std::free(c_pattern);
		return error;
	}
	if((error = posix_spawn_file_actions_adddup2(&file_actions, outfd, 1)) != 0){
		posix_spawn_file_actions_destroy(&file_actions);
		std::free(c_pattern);
		return error;
	}
	
	/* posix_spawn */
	error =
		posix_spawn(
			pid, argv[0], &file_actions, nullptr, const_cast<char * const *>(argv), environ
		);
	posix_spawn_file_actions_destroy(&file_actions);
	std::free(c_pattern);
	return error;
}

static int read_0(
	FILE *in, std::function<int (std::string_view)> f,
	std::size_t *buffer_capacity, std::size_t *buffer_length, char **buffer_data
)
{
	char c;
	int r = std::fread(&c, 1, 1, in);
	if(r != 1){
		if(std::ferror(in)) return errno;
		if(std::feof(in)) return EOF; /* -1 */
	}else if(c == '\0'){
		int error = f(std::string_view(*buffer_data, *buffer_length));
		if(error != 0) return error;
		*buffer_length = 0;
	}else{
		if(*buffer_length >= *buffer_capacity){
			char *new_data =
				static_cast<char *>(std::realloc(*buffer_data, *buffer_capacity * 2));
			if(new_data == nullptr) return errno;
			*buffer_capacity = malloc_usable_size(new_data);
			*buffer_data = new_data;
		}
		(*buffer_data)[*buffer_length] = c;
		++ *buffer_length;
	}
	return 0;
}

int locate(
	std::string_view pattern, bool base_name, bool ignore_case,
	std::function<int (std::string_view)> f, int *status
)
{
	int error;
	int pending_error = 0;
	
	/* pipe */
	int pipefds[2];
	if(pipe(pipefds) < 0) return errno;
	if((error = set_cloexec(pipefds[0])) != 0) return error;
	FILE *in = fdopen(pipefds[0], "r");
	if(in == nullptr){
		error = errno;
		do_close(pipefds[0]);
		do_close(pipefds[1]);
		return error;
	}
	
	/* spawn */
	int pid;
	if(
		(error = spawn_locate(pattern, base_name, ignore_case, pipefds[1], &pid)) != 0
	){
		std::fclose(in);
		do_close(pipefds[1]);
		return error;
	}
	if((error = do_close(pipefds[1])) != 0){
		pending_error = error;
	}
	
	/* read */
	char *buffer_data = static_cast<char *>(std::malloc(PATH_MAX));
	if(buffer_data == nullptr){
		pending_error = errno;
	}else{
		std::size_t buffer_capacity = malloc_usable_size(buffer_data);
		std::size_t buffer_length = 0;
		while(pending_error == 0 && ! std::feof(in)){
			error = read_0(in, f, &buffer_capacity, &buffer_length, &buffer_data);
			if(error != 0 && error != EOF){
				pending_error = error;
			}
		}
		free(buffer_data);
	}
	
	/* wait */
	do{
		if((error = do_waitpid(pid, status, 0)) != 0){
			std::fclose(in);
			return error;
		}
	}while(! WIFEXITED(*status) && ! WIFSIGNALED(*status));
	if(std::fclose(in) < 0) return errno;
	if((WIFEXITED(*status) && WEXITSTATUS(*status) != 0) || WIFSIGNALED(*status)){
		return ELOCATE_FAILURE;
	}
	
	return pending_error;
}
