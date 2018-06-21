#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "tokenizer.h"

#include <sys/resource.h>
#include <sys/time.h>

#include <fcntl.h>
/* Convenience macro to silence compiler warnings about unused function parameters. */
#define unused __attribute__((unused))


/* Whether the shell is connected to an actual terminal or not. */
bool shell_is_interactive;

/* File descriptor for the shell input */
int shell_terminal;

/* Terminal mode settings for the shell */
struct termios shell_tmodes;

char** shell_variable_names;
char** shell_variable_values;
/* Process group id for the shell */
pid_t shell_pgid;


int lookup(char cmd[]);

void pipeline(char * args[], int background, int niceness, int nice_n);

int run_program(char * args[], int background, int niceness, int nice_n);

// io == -1, parsing error, io == 0 is no io, io == 1 is read, io == 2 is write, io , io == 3 is write with append, io == 4 is read and write, io == 5 is read and write with append
int get_IO(char * args[], char ** readfilename, char ** writefilename, char *** augx, int *len);

// int set_variable(char * line);

int parse(char * line);

int declare_variable(char* arg);
int put_in_shell(char* a, char* b);
int find_in_shell_variables(char* value);

int sigint_called;
/* last child proccess status code */
int last_childproccess_status_code = 0;

void set_signal_for_child();
void set_signal();

int cmd_exit(char * args[]);
int cmd_help(char * args[]);
int cmd_pwd(char * args[]);
int cmd_cd(char * args[]);
int cmd_kill(char * argsp[]);
int cmd_ulimit(char * args[]);
int cmd_export(char* args[]);
int cmd_echo(char * args[]);
int cmd_type(char * args[]);
int cmd_nice(char * args[]);

/* Built-in command functions take token array (see parse.h) and return int */
typedef int cmd_fun_t(char * args[]);

typedef struct fun_desc {
  cmd_fun_t *fun;
  char *cmd;
  char *doc;
} fun_desc_t;

fun_desc_t cmd_table[] = {
  {cmd_help, "?", "show this help menu"},
  {cmd_exit, "exit", "exit the command shell"},
  {cmd_pwd, "pwd", "show current working directory"},
  {cmd_cd, "cd", "change current working directory"},
  {cmd_echo, "echo", "prints varname, child proccess status or some string"},
  {cmd_kill, "kill", "kills a target process"},
  {cmd_ulimit, "ulimit", "get and set user limits"},
  {cmd_export, "export", "exports given variable"},
  {cmd_type, "type", "pritns type"},
  {cmd_nice, "nice", "prints niceness or changes it"},
};

typedef struct ulimit_t {
	char* parameter;
	int RESOURCE;
	char* description;
} ulimit_t;

ulimit_t ulimit_table[]={ // two more to be added as soon as i find their Ints
  {"-c", RLIMIT_CORE,"core file size"},
  {"-d", RLIMIT_DATA, "data seg size"},
  {"-e", RLIMIT_NICE, "scheduling priority"},
  {"-f", RLIMIT_FSIZE, "file size"},
  {"-i", RLIMIT_SIGPENDING, "pending signals"},
  {"-l", RLIMIT_MEMLOCK, "max locked memory"},
  {"-n", RLIMIT_NOFILE, "open files"},
  {"-q", RLIMIT_MSGQUEUE, "POSIX message queues"},
  {"-r", RLIMIT_RTPRIO, "real-time priority"},
  {"-s", RLIMIT_STACK,"stack size"},
  {"-t", RLIMIT_CPU, "cpu time"},
  {"-u", RLIMIT_NPROC, "max user processes"},
  {"-v", RLIMIT_AS,"virtual memory"},
  {"-x", RLIMIT_LOCKS, "file locks"},
  {"-a", 0, NULL},
};


int remove_white_space(char * line, char *args[], int export){
	char * delims = " \n\t";
	int need_comm = 0;
	if (strstr(line, "-c ") || strstr(line, "echo") == line) need_comm = 1;
	if(!export){
		int flag = 0;
		for(int i = 0; i < strlen(line); i++){
			if(line[i] == '\'' || line[i] == '\"'){
				if(flag == 0){
					flag = 1;
				}else{
					flag = 0;
				}
			}
			if(flag == 1){
				if(!need_comm && isspace(line[i])){
					for(int j = i; j < strlen(line); j++)
						line[j] = line[j+1];
					}
			}
		}
		
		delims = " \n\t\'\"";
		if((args[0] = strtok(line, delims)) == NULL) 
   	 return -1;

		int pos = 1;	
	  while((args[pos] = strtok(NULL, delims)) != NULL)
		{
			if (need_comm && pos == 1)
				delims = "\n\"\'";

	  	pos++;
		}
		
		return pos - 1;
	}

	if((args[0] = strtok(line, delims)) == NULL) 
    return -1;

  int pos = 1;
  while((args[pos] = strtok(NULL, "\n\t")) != NULL)
    pos++;

  return pos - 1;
}

int get_commands(char * line, char *** commands) {
	char * line2 = strdup(line);
	char * temp[256];
	if(remove_white_space(line2, temp, 0) <= -1)
		return -1;
	int commands_pos = 0;
	int curr_pos = 0;
	char *curr = malloc(4096);
	char * res[4096]; 
	memset(curr, '\0', 4096);
	for(int i = 0; i < strlen(line); i++){ //todo remove -1
		if(line[i] == '&' && line[i+1] != '\0' && line[i+1] == '&'){
			curr[curr_pos] = '\0';
			
			res[commands_pos++] = curr;
			res[commands_pos++] = "&&";
			
			curr = (char *)malloc(4096);
			curr_pos = 0;
			i++;
		}else if(line[i] == '|' && line[i+1] != '\0' && line[i+1] == '|'){
			curr[curr_pos] = '\0';
			
			res[commands_pos++] = curr;
			res[commands_pos++] = "|";
			
			
			curr = (char *)malloc(4096);
			curr_pos = 0;
			i++;
		}else if(line[i] == ';'){
			curr[curr_pos] = '\0';
			
			res[commands_pos++] = curr;
			res[commands_pos++] = ";";	
			
			
			curr = (char *)malloc(4096);
			curr_pos = 0;
		}else{
			curr[curr_pos++] = line[i];
//			printf("%s\n", curr);
		}
	}

	res[commands_pos++] = curr;

	free(curr);
	*commands = res;
	return commands_pos;
}

void pipeline(char * args[], int background, int niceness, int nice_n){

	int fd[2]; 
	int fd2[2];
	
	int num_cmds = 0;
	
	char *command[4096];
	
	pid_t pid;
	pid_t group_pid;
	
	int end = 0;
	
	
	int i = 0;
	int j = 0;
	int k = 0;
	int l = 0;

	while (args[l] != NULL){
		if (strcmp(args[l],"|") == 0){
			num_cmds++;
		}
		l++;
	}
	num_cmds++;

	pid_t pidarr[num_cmds];
	
	while (args[j] != NULL && end != 1){
		k = 0;
		while (strcmp(args[j],"|") != 0){
			command[k] = args[j];
			j++;	
			if (args[j] == NULL){
				end = 1;
				k++;
				break;
			}
			k++;
		}
		command[k] = NULL;
		j++;		

		if (i % 2 != 0){
			pipe(fd);
		}else{
			pipe(fd2);
		}
		
		pid=fork();
		
		if(pid==-1){			
			if (i != num_cmds - 1){
				if (i % 2 != 0){
					close(fd[1]);
				}else{
					close(fd2[1]);
				} 
			}			
			printf("Child process could not be created\n");
			return;
		}
		if(pid==0){
			
			if (i == 0){
				group_pid = getpid();
    		setpgid(getpid(), group_pid);
				dup2(fd2[1], STDOUT_FILENO);
			}
			else if (i == num_cmds - 1){
				setpgid(getpid(), group_pid);
				if (num_cmds % 2 != 0){ 
					dup2(fd[0],STDIN_FILENO);
				}else{ 
					dup2(fd2[0],STDIN_FILENO);
				}
			}else{
				setpgid(getpid(), group_pid);
				if (i % 2 != 0){
					dup2(fd2[0],STDIN_FILENO); 
					dup2(fd[1],STDOUT_FILENO);
				}else{ 
					dup2(fd[0],STDIN_FILENO); 
					dup2(fd2[1],STDOUT_FILENO);					
				}
			}

			if(i != num_cmds-1)
				pidarr[i] = getpid();

			int len = 0;
			char * readfilename = "";
			char * writefilename = "";
		  char **augx ;
		  int io = get_IO(command, &readfilename, &writefilename, &augx, &len);

		  if(io == -1){
		  	printf("Parsing error\n");
		  	return ;
		  }

		  if(len <= 0)
		  	return ;

			set_signal_for_child();

			/* START */

			if(i==0){
				if(strcmp(augx[0], "nice") == 0)
					augx = &augx[1];
				if(strcmp(augx[0], "-n") == 0)
					augx = &augx[2];
			}

		  char * env = getenv("PATH");

		  char *program_directory = augx[0];
		  
		  if (access(program_directory, F_OK) == -1) {

			  char curr[1024];
			  int pos = 0;
			  for(int i = 0; env[i] != '\0'; i++){
			  	if(env != NULL){
				  	if(env[i] == ':'){
				  		curr[pos] = '/';
				  		curr[pos + 1] = '\0';
				  		strcat(curr, program_directory);
				  		if(access(curr, F_OK) != -1){
								program_directory = curr;
				  			break;
				  		}
				  		pos = 0;
				  		continue;
				  	}
				  	curr[pos] = env[i];
				  	pos++;
				  }
				}
			  if (access(program_directory, F_OK) == -1){
			    printf("%s : command not found\n", program_directory);
			    return ;
			  }
		  }
		  
		  char* new_args[len + 1];

		  new_args[0] = program_directory;

		  for (int i = 1; i < len; i++) {
		    new_args[i] = augx[i];
		  }
		  new_args[len] = NULL;

	  	int fd;
	  	// io == -1, parsing error, io == 0 is no io, io == 1 is read, io == 2 is write, io , io == 3 is write with append, io == 4 is read and write, io == 5 is read and write with append
	  	if(io == 1){
	  		fd = open(readfilename, O_RDONLY, 0600);
	  		if(fd < 0){
	  			printf("Wrong file name\n");
	  			kill(pid, SIGTERM);
	  			return ;
	  		}
	  		dup2(fd, STDIN_FILENO);
				close(fd);
	  		
	  	}else if(io == 2){
	  		fd = open(writefilename, O_CREAT | O_WRONLY | O_TRUNC, 0600);
	  		
	  		dup2(fd, STDOUT_FILENO); 
				close(fd);
	  	}else if(io == 3){
	  		fd = open(writefilename, O_CREAT | O_WRONLY | O_APPEND, 0600);
	  		dup2(fd, STDOUT_FILENO);
				close(fd);
	  	}else if (io == 4){
	  		fd = open(readfilename, O_RDONLY, 0600);
	  		if(fd < 0){
	  			printf("Wrong file name\n");
	  			kill(pid, SIGTERM);
	  			return ;
	  		}
	  		dup2(fd, STDIN_FILENO);
				close(fd);

				fd = open(writefilename, O_CREAT | O_WRONLY | O_TRUNC, 0600);

	  		dup2(fd, STDOUT_FILENO); 
				close(fd);

	  	}else if(io == 5){
	  		fd = open(readfilename, O_RDONLY, 0600);
	  		if(fd < 0){
	  			printf("Wrong file name\n");
	  			kill(pid, SIGTERM);
	  			return ;
	  		}
	  		dup2(fd, STDIN_FILENO);
				close(fd);

				fd = open(writefilename, O_CREAT | O_WRONLY | O_APPEND, 0600);
	  		
	  		dup2(fd, STDOUT_FILENO); 
				close(fd);

	  	}

	  	if(background == 0)
      	tcsetpgrp(shell_terminal, group_pid);

      if(i == 0 && niceness)
      	nice(nice_n);

	    execv(program_directory, new_args);
		}else{
			if(i == 0)
				group_pid = pid;
    	setpgid(pid, group_pid);
				if(background == 0){
					tcsetpgrp(shell_terminal, group_pid);
					if(i == num_cmds - 1){
						waitpid(pid, &last_childproccess_status_code, WUNTRACED);
					}
					tcsetpgrp(shell_terminal, shell_pgid);
				
				}else{
					printf("Background proccess with pid:%d\n", pid);
				}
		}

		if (i == 0){
			close(fd2[1]);
		}
		else if (i == num_cmds - 1){
			if (num_cmds % 2 != 0){					
				close(fd[0]);
			}else{					
				close(fd2[0]);
			}
		}else{
			if (i % 2 != 0){					
				close(fd2[0]);
				close(fd[1]);
			}else{					
				close(fd[0]);
				close(fd2[1]);
			}
		}
				
		i++;	
	}
	
	for(int i = 0; i < num_cmds - 1; i++)
		waitpid(pidarr[i], NULL, WUNTRACED);
}

int parse(char * line){
	//printf("%s\n", line);
	char ** commands;
	int built_in_last = -1;
	int built_in_status = -1;
	int background = 0;
	
	int count = get_commands(line, &commands);
	if(count <= 0)
		return -1;
	
	for(int j = 0; commands[count - 1][j] != '\0'; j++){
		if(commands[count - 1][j] == '&' && commands[count - 1][j+1] != commands[count - 1][j]){
			background = 1;
			commands[count - 1][j] = '\0';
			break;
		}
	}
	
	for(int pos = 0; pos < count ; pos++){
		if (strstr(commands[pos], "=") && strstr(commands[pos], "export") == NULL) {
			char* tmp = malloc(strlen(commands[0]) + 1);
			
			built_in_last = 1;
      built_in_status = declare_variable(strcpy(tmp, commands[0]));	
      continue;
		}
		if(strcmp(commands[pos], "&&") == 0){
			if(built_in_last == 1){
				if(built_in_status >= 0)
					continue;
				else pos++;
			}
			if(last_childproccess_status_code == 0)
				continue;
			else pos++;
		}else if(strcmp(commands[pos], "|") == 0){
			if(built_in_last == 1){
				if(built_in_status < 0)
					continue;
				else pos++;
			}
			if(last_childproccess_status_code != 0)
				continue;
			else pos++;
		}else if(strcmp(commands[pos], ";") == 0){
			char * ret = strstr(line, ";");
			char * args[256];
			if(remove_white_space(ret, args, 0) <= 0)
				break;
			else continue;
		}else{
			char *args[256];
			int argsLength ;
			int i = 0;
			if(strstr(commands[pos], "export"))
				argsLength = remove_white_space(commands[pos], args, 1);
			else
				argsLength = remove_white_space(commands[pos], args, 0);

			int fundex = lookup(args[0]);
			int broke_from_inner = 0;
			int niceness = 0;
			int nice_n = 0;
		
			if (fundex >= 0 && strcmp(args[0], "nice") != 0) {
				built_in_last = 1;
      	built_in_status = cmd_table[fundex].fun(args);	
			}else {
				if(fundex >= 0){
					niceness = 1;
					nice_n = cmd_table[fundex].fun(args);
					if(nice_n < 0){
						built_in_last = 1;
      			built_in_status = nice_n;
						continue;
					}else if(args[1] == NULL){
						built_in_last = 1;
						built_in_status = 0;
						continue;
					}
				}
				built_in_last = 0;
				if (args[argsLength][0] == '&' ){
						printf("Parsing error, incorect declaration of &\n");
						return -1;
				}
				while (args[i] != NULL){
					if (strcmp(args[i],"|") == 0){
						broke_from_inner = 1;
						pipeline(args, background, niceness, nice_n);
						break;
					}
					i++;
				}
				if(broke_from_inner == 1){
					int l = 0;
					while(args[l] != NULL){
						memset(args[l], '\0', 256);
						l++;
					}
					//printf("%ld\n", sizeof(args));
					continue;
				}
				run_program(args, background, niceness, nice_n);
				int l = 0;
					while(args[l] != NULL){
						memset(args[l], '\0', 256);
						l++;
					}
			}
		}
	}

	//free(commands);
	return 1;

}

// io == -1, parsing error, io == 0 is no io, io == 1 is read, io == 2 is write, io , io == 3 is write with append, io == 4 is read and write, io == 5 is read and write with append
int get_IO(char * args[], char ** readfilename, char ** writefilename, char *** augx, int *len){
	//int fd = open("bla.txt", O_RDONLY);
	//if(fd < 0)
		//return -1;
	int ret = 0;
	int pos = 0;
	char *res[4096];

	for(int i = 0; args[i] != NULL; i++){
		for(int j = 0; j < strlen(args[i]); j++){
			if(args[i][j] == '>'){
					if(args[i][j+1] != '\0' && args[i][j+1] == '>'){
						if(args[i][j+2]!='\0'){
							char fn[256];
							int k;
							for(k = j + 2; k < strlen(args[i]); k++){
								fn[k - j - 2] = args[i][k];
							}
							fn[k - j - 2] = '\0';
							*writefilename = fn;
							if(ret == 1)
								ret = 5;
							else
								ret = 3;
							j++;
							continue;
						}else{
							if(args[i+1] == NULL)
								return -1;
							*writefilename = args[i+1];
							if(ret == 1)
								ret = 5;
							else
								ret = 3;
							j++;
							continue;
						}
					}
					if(args[i][j+1]!='\0'){
						char fn[256];
						int k;
						for(k = j + 1; k < strlen(args[i]); k++){
							fn[k - j - 1] = args[i][k];
						}
						fn[k - j - 1] = '\0';
						*writefilename = fn;
					}else{
						if(args[i+1] == NULL)
							return -1;
						*writefilename = args[i+1];
					}
					if(ret == 1)
						ret = 4;
					else
						ret = 2;
					continue;
				}else if(args[i][j] == '<'){
					if(args[i][j+1]!='\0'){
						char fn[256];
						int k;
						for(k = j + 1; k < strlen(args[i]); k++){
							fn[k - j - 1] = args[i][k];
						}
						fn[k - j - 1] = '\0';
						*readfilename = fn;
					}else{
						if(args[i+1] == NULL)
							return -1;
						*readfilename = args[i+1];
					}
					if(ret == 2)
						ret = 4;
					else if(ret == 3)
						ret = 5;
					else
						ret = 1;
					continue;
				}
		}
		
	}

	for(int i = 0; args[i] != NULL; i++){
		if( (strcmp(*readfilename, args[i]) != 0 && strcmp(*writefilename, args[i]) != 0 ) && args[i][0] != '>' && args[i][0] != '<'){
			for(int k = 0; k < strlen(args[i]); k++)
				if(args[i][k] == '>' || args[i][k] == '<')
					args[i][k] = '\0';
			res[pos++] = args[i];
			(*len)++;
		}

	}

	*augx = res;
	return ret;
}

/* runs the program specified by input */
int run_program(char * args[], int background, int niceness, int nice_n) {
	if(strcmp(args[0], "nice") == 0)
		args = &args[1];
	if(strcmp(args[0], "-n") == 0)
		args = &args[2];
	int len = 0;
	char * readfilename = "";
	char * writefilename = "";
  char **augx ;
  int io = get_IO(args, &readfilename, &writefilename, &augx, &len);
  
  if(io == -1){
  	printf("Parsing error\n");
  	return -1;
  }

  if(len <= 0)
  	return -1;

  pid_t pid ;
  pid_t group_pid;

  char * env = getenv("PATH");

  char *program_directory = augx[0];

  if (access(program_directory, F_OK) == -1) {
		if(env != NULL){
		  char curr[1024];
		  int pos = 0;

		  for(int i = 0; env[i] != '\0'; i++){
		  	if(env[i] == ':'){
		  		curr[pos] = '/';
		  		curr[pos + 1] = '\0';
		  		strcat(curr, program_directory);
		  		if(access(curr, F_OK) != -1){
						program_directory = curr;
		  			break;
		  		}
		  		pos = 0;
		  		continue;
		  	}
		  	curr[pos] = env[i];
		  	pos++;
		  }
		}
	  if (access(program_directory, F_OK) == -1){
	    printf("%s : command not found\n", program_directory);
	    return -1;
	  }
  }
  
  char* new_args[len + 1];

  new_args[0] = program_directory;

  for (int i = 1; i < len; i++) {
    new_args[i] = augx[i];
  }
  new_args[len] = NULL;
  char** tmp[2];
  tmp[0] = shell_variable_names;
  tmp[1] = shell_variable_values;
  shell_variable_names = NULL;
  shell_variable_values = NULL;
  pid = fork();
  

  if (pid < 0) {
    fprintf(stderr, "Fork Failed");
    return 1;
  }else if (pid > 0) {
  	shell_variable_names = tmp[0];
  	shell_variable_values = tmp[1];
  	group_pid = pid;
    setpgid(pid, group_pid);
    if(background == 0){
    	tcsetpgrp(shell_terminal, group_pid);
    	
    	waitpid(group_pid, &last_childproccess_status_code, WUNTRACED);
    	tcsetpgrp(shell_terminal, shell_pgid);
    }
    else{
    	printf("Background proccess with pid:%d\n", pid);
    }
  }else {
  	group_pid = getpid();
  	setpgid(getpid(), group_pid);
  	set_signal_for_child();

  	int fd;
  	// io == -1, parsing error, io == 0 is no io, io == 1 is read, io == 2 is write, io , io == 3 is write with append, io == 4 is read and write, io == 5 is read and write with append
  	if(io == 1){
  		fd = open(readfilename, O_RDONLY, 0600);
  		if(fd < 0){
  			printf("Wrong file name\n");
  			kill(pid, SIGTERM);
  			return -1;
  		}
  		dup2(fd, STDIN_FILENO);
			close(fd);
  		
  	}else if(io == 2){
  		fd = open(writefilename, O_CREAT | O_WRONLY | O_TRUNC, 0600);
  		
  		dup2(fd, STDOUT_FILENO); 
			close(fd);
  	}else if(io == 3){
  		fd = open(writefilename, O_CREAT | O_WRONLY | O_APPEND, 0600);
  		dup2(fd, STDOUT_FILENO);
			close(fd);
  	}else if (io == 4){
  		fd = open(readfilename, O_RDONLY, 0600);
  		if(fd < 0){
  			printf("Wrong file name\n");
  			kill(pid, SIGTERM);
  			return -1;
  		}
  		dup2(fd, STDIN_FILENO);
			close(fd);

			fd = open(writefilename, O_CREAT | O_WRONLY | O_TRUNC, 0600);
  		
  		dup2(fd, STDOUT_FILENO); 
			close(fd);

  	}else if(io == 5){
  		fd = open(readfilename, O_RDONLY, 0600);
  		if(fd < 0){
  			printf("Wrong file name\n");
  			kill(pid, SIGTERM);
  			return -1;
  		}
  		dup2(fd, STDIN_FILENO);
			close(fd);

			fd = open(writefilename, O_CREAT | O_WRONLY | O_APPEND, 0600);
  		
  		dup2(fd, STDOUT_FILENO); 
			close(fd);

  	}
  	
  	if(background == 0)
  		tcsetpgrp(shell_terminal, group_pid);

  	if(niceness)
  		nice(nice_n);

    execv(program_directory, new_args);

  //  return 0;
  }

  return 0;
}

/* prints varname, child proccess statud or some string */
int cmd_echo(char * args[]){
	int len = 0;
	while(args[len] != NULL)len++;
	if(len >= 2){
		for(int i = 1; args[i] != NULL; i++){
			char * curr = args[i];
			if(curr[0] == '$' && curr[1] != '\0'){
				if(curr[1] != '?'){
					char * env = getenv(curr + 1);
					if(env != NULL){
						printf("%s", env);
				//		return 0;
						}
					else {									
						int i = find_in_shell_variables(curr +1);
				//		printf("%d", i);
						if (i != -1) {
							printf("%s", shell_variable_values[i]);
				//			return 0;
						}
					}
				}else {
					printf("%d", last_childproccess_status_code);
					printf("%s ", curr + 2);
				}
			}else{
				printf("%s ", curr);
			}
		}
	}

	printf("\n");
	return 0;
}

/* Prints a helpful description for the given command */
int cmd_help(char * args[]) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    printf("%s - %s\n", cmd_table[i].cmd, cmd_table[i].doc);
  return 0;
}

/* Exits this shell */
int cmd_exit(char * args[]) {
	if(args[1] != NULL)
  	exit(atoi(args[1]));
  else
  	exit(0);
  return 0;
}

/* displays working directory */
int cmd_pwd(char * args[]) {
  char current_dir[1024];
  getcwd(current_dir, 1024);
  printf("%s\n", current_dir);
  return 0;
}

/* Changes current working directory */
int cmd_cd(char * args[]) {
  //check that an argument was passed
  int len = 0;
  while(args[len] != NULL)
  	len++;
  if (len < 2) {
    printf("usage: cd DIRECTORY\n");
    return -1;
  }
  int chdir_result = chdir(args[1]);
  if (chdir_result != 0) {
    printf("error changing directory\n");
    return -1;
  }
  return 0;
}

int cmd_kill(char* args[]) {
	if (args[1] == NULL) {
		printf("kill: usage: kill [-s sigspec | -n signum | -sigspec] pid\n");
		return -1;
	}
	int signal = SIGTERM;
	int i = 1;
	if (args[1][0] == '-') {
		signal = atoi(args[i++] + 1);
	}

	for (; args[i] != NULL; i++) {
		pid_t proc = atoi(args[i]);
		kill(proc, signal);
	}
	
	return 0;
}

int cmd_type(char * args[]) {
	if (args[1] == NULL) return 0;
	int find_all = 0;
	if(args[1][0] == '-')
		find_all = 1;
	
	int ret = 0;
	
	for(int i = find_all + 1; args[i] != NULL; i++){
		int found = 0;
		int fundx = lookup(args[i]);
		if(fundx >= 0){
			printf("%s is a shell built-in\n", args[i]);
			found = 1;
			if(!find_all)
				continue;
		}
		
		char * env = getenv("PATH");

		char *program_directory = args[i];

		if(env != NULL){
		  char curr[1024];
		  int pos = 0;

		  for(int i = 0; env[i] != '\0'; i++){
		  	if(env[i] == ':'){
		  		curr[pos] = '/';
		  		curr[pos + 1] = '\0';
		  		strcat(curr, program_directory);
		  		if(access(curr, F_OK) != -1){
						program_directory = curr;
						found = 1;
		  			break;
		  		}
		  		pos = 0;
		  		continue;
		  	}
		  	curr[pos] = env[i];
		  	pos++;
		  }
		}
		if(found == 0){
			ret = -1;
			printf("%s: not found\n" , args[i]);
		}else
			printf("%s is a %s\n", args[i], program_directory);
	}
	
	return ret;
}

char *trim(char *str)
{
  char *end;
  //printf("%s", str);

  // Trim leading space
  while(isspace((unsigned char)*str) || str[0] == '\'' || str[0] == '\"') str++;
  if(*str == 0)  // All spaces?
    return str;

  // Trim trailing space
  end = str + strlen(str) - 1;
  while(end > str && (isspace((unsigned char)*end) || end[0] == '\'' || end[0] == '\"')) end--;
  
  int size = end - str + 2;
  char* res = malloc(size);
  strncpy(res, str, size - 1);

  // Write new null terminator
  *(res + size - 1) = 0;

  return res;
}

int find_in_shell_variables(char* var_name) {
	for (int i = 0; i < 256; i++) {
		if (shell_variable_names[i] != 0 && strcmp(var_name, shell_variable_names[i]) == 0) {
			return i;
		}
	}
	return -1;
}

int cmd_export(char* args[]) {
	if (args[1] == NULL) {
		return -1;
	}
	int i;
	//printf("%s", args[1]);
	if (strstr(args[1], "=")) {
		i = declare_variable(args[1]);
	}
	else
		i = find_in_shell_variables(args[1]);
	if (i != -1) {
		setenv(shell_variable_names[i], trim(shell_variable_values[i]), 1);
		free(shell_variable_names[i]);
		free(shell_variable_values[i]);
		shell_variable_names[i] = 0;
		shell_variable_values[i] = 0;
		return 0;
	}
	return -2;
}

int declare_variable(char* arg) {
  int i = 0;
	for (; i < strlen(arg); i++) {
		if (arg[i] == '=')
			break;
	}
//	printf("%d", i);
  char* a = malloc(i + 1);
  char* b = malloc(strlen(arg) - i);
	strncpy(a, arg, i);
	a[i] = '\0';
	strncpy(b, arg + i + 1, strlen(arg) - i - 1);
	b[strlen(arg) - i - 1] = '\0';
	if (getenv(a) != NULL) {
		setenv(a, trim(b), 1);
		free(a);
		free(b);
		return -1;
	}

  return put_in_shell(a, trim(b));
}

int put_in_shell(char* a, char* b) {
//	printf("%s %s", a, b);
	for (int i = 0; i < 256; i++) {
		if (shell_variable_names[i] != 0 && strcmp(a, shell_variable_names[i]) == 0) {
			free(shell_variable_values[i]);
			shell_variable_values[i] = b;
			return i;
		}
	}
	for (int i = 0; i < 256; i++) {
		if (shell_variable_values[i] == 0) {
			shell_variable_names[i] = a;
			shell_variable_values[i] = b;
			return i;
		}
	}
	return -1;
}

int search_for_resource(char* parameter) {
	for (int i = 0; i < 14; i++) {
		char* temp = ulimit_table[i].parameter;
		if (strcmp(temp, parameter) == 0) {
			return ulimit_table[i].RESOURCE;
		}
	}
	return -1;
}

int list_limits() {  
	struct rlimit limits;
	for (int i = 0; i < 14; i++) {
		getrlimit(ulimit_table[i].RESOURCE, &limits);
		if (limits.rlim_cur == -1) 
			printf("%-20s (%2s) %-20s\n", ulimit_table[i].description,  ulimit_table[i].parameter, "unlimited");
		else
			printf("%-20s (%2s) %-20ju\n", ulimit_table[i].description,  ulimit_table[i].parameter, limits.rlim_cur);		
	}
	return 0;
}

int cmd_ulimit(char * args[]) {
	int RESOURCE = RLIMIT_FSIZE;
  if (args[1] != NULL) {
  	if (strcmp(args[1], "-a") == 0)
   		return list_limits();
   	RESOURCE = search_for_resource(args[1]);
   	if (RESOURCE == -1) {
   		printf("no such limit\n");
   		return -1;
  	}
  }
  struct rlimit limits;
  getrlimit(RESOURCE, &limits);
  if (args[1] == NULL || args[2] == NULL) {
  	if (limits.rlim_cur == RLIM_INFINITY)
  		printf("unlimited\n");
  	else 
  		printf("%ju\n", limits.rlim_cur);
  } else {
 		int new_limit = atoi(args[2]);
 		limits.rlim_cur = new_limit;
	 	setrlimit(RESOURCE, &limits);
  }
    
	return 0; 
}

/* Looks up the built-in command, if it exists. */
int lookup(char cmd[]) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0))
      return i;
  return -1;
}

void set_signal(){
	signal(SIGINT, SIG_IGN);
	signal(SIGTERM, SIG_IGN);
	signal(SIGTTIN, SIG_IGN);
	signal(SIGTTOU, SIG_IGN);
	signal(SIGTSTP, SIG_IGN);
}

void set_signal_for_child(){
	signal(SIGINT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGTTIN, SIG_DFL);
	signal(SIGTTOU, SIG_DFL);
	signal(SIGTSTP, SIG_DFL);
}

void send_background(int p){
	while (waitpid(-1, &last_childproccess_status_code, WNOHANG) > 0) {
	}
	//printf("\n");
}

/* Intialization procedures for this shell */
void init_shell() {
  /* Our shell is connected to standard input. */
  shell_terminal = STDIN_FILENO;

  sigint_called = 0;

  /* Check if we are running interactively */
  shell_is_interactive = isatty(shell_terminal);

  if (shell_is_interactive) {
    /* If the shell is not currently in the foreground, we must pause the shell until it becomes a
     * foreground process. We use SIGTTIN to pause the shell. When the shell gets moved to the
     * foreground, we'll receive a SIGCONT. */
    while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
      kill(-shell_pgid, SIGTTIN);

    /* Saves the shell's process id */
    shell_pgid = getpid();

    signal(SIGCHLD, send_background);
    set_signal();

    /* Take control of the terminal */
    tcsetpgrp(shell_terminal, shell_pgid);

    /* Save the current termios to a variable, so it can be restored later. */
    tcgetattr(shell_terminal, &shell_tmodes);
    
    shell_variable_names = malloc(sizeof(char*) * 256);
    memset(shell_variable_names, 0, sizeof(char*));
    shell_variable_values = malloc(sizeof(char*) * 256);
    memset(shell_variable_values, 0, sizeof(char*));
  }
  
}

int cmd_nice(char * args[]){
	if(args[1] != NULL){
		if(strcmp(args[1], "-n") == 0){
			if(!isdigit(args[2][0]) && args[2][0] != '-' ){
				printf("parsing error\n");
				return -1;
			}else {
				if(args[2][0] == '-'){
					return 0;
				}else{
					return atoi(args[2]);
				}
			}
		}else{
			return 10;
		}
	}else{
		printf("%d\n", getpriority(PRIO_PROCESS, shell_pgid));
		return 0;
	}

	return 1;
}

int main(unused int argc, unused char *argv[]) {
  init_shell();
  static char line[4096];
  int line_num = 0;
  
  /* Please only print shell prompts when standard input is not a tty */
  if (shell_is_interactive)
    fprintf(stdout, "%d: ", line_num);
	
	if (argc > 1 && !strcmp(argv[1], "-c")) {
	/*	int i = 0;
		for (int j = 2; j < argc; j++) {
			strcpy(&line[i], argv[j]);
			i += strlen(argv[j] + 1);
		} */
		//char* initial_command = trim(line);
		parse(argv[2]);
	}
	
  while (fgets(line, 4096, stdin)) {
    /* Split our line into words. */
    struct tokens *tokens = tokenize(line);



    /* Find which built-in function to run. */
//    int fundex = lookup(tokens_get_token(tokens, 0));

//    if (fundex >= 0) {
//      cmd_table[fundex].fun(tokens);
//    } else {
    	parse(line);
    	//if(set_variable(line) == 1)
      //	frogram(tokens);
//    }

    if (shell_is_interactive)
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);

    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}
