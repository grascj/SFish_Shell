#include "sfish.h"

#define SAFE_PRINTER(fd,...) safe_print(fd, __VA_ARGS__, NULL)

typedef enum {SYNC,ASYNC} PRINTER;
const size_t builtin_pairs_size = 12;

//globals
int curr_fd=STDOUT_FILENO;

//return value
int retvalue=0;

//current directory
char * current_working_directory = NULL;
size_t cwd_size = 256;

//user prompt
char * prompt_str = NULL;
size_t prompt_str_size = 128;
prompt USER_PROMPT = {1,1,-1,-1,0,0};

int pipe_num;
int dispose;
sigset_t mask;
job* CURRENT_JOB = NULL;
job *joblist_head = NULL;
size_t NUM_COMMANDS = 0;
job*SPID = NULL;

int main(int argc, char** argv) {
	//This is disable readline's default signal handlers, since you are going
	rl_catch_signals = 0;
	
	//READLINE HOTKEYS
	rl_bind_keyseq("\\C-h",help_handler);
	rl_bind_keyseq("\\C-b",store_pid_handler);
	rl_bind_keyseq("\\C-g",get_pid_handler);
	rl_bind_keyseq("\\C-p",sfish_info_handler);
	
	char *cmd;

	//mask children
	sigemptyset(&mask);
	sigaddset(&mask,SIGCHLD);
	signal(SIGPIPE,SIG_IGN);
	signal(SIGINT, SIG_IGN);
	signal(SIGCHLD,child_daycare);
	signal(SIGTTOU,SIG_IGN);
	signal(SIGTTIN,SIG_IGN);

	prompt_update(FULL_CHANGE);

w_uber:
	//loop for each command
	while(sigprocmask(SIG_UNBLOCK, &mask,NULL),(cmd = readline(prompt_str)) != NULL) {



		sigprocmask(SIG_BLOCK, &mask, NULL);



		//STRING PARSING
		
		if(cmd[0] == '\0'){ //empty input
			child_daycare(0);
			job_report(SYNC);
			goto w_uber;
		}

		int bg_flag = 0;
		bg_flag = 0;

		char* k;
		if(k = contains_at_end(cmd,'&'),k != NULL)
		{
			bg_flag=1;
			*k = '\0';
		}
		char * cmd_reset = malloc((strlen(cmd)+1)*sizeof(char));
		char* cmd_reset_keeper = cmd_reset;
		strcpy(cmd_reset,cmd);

		char** piped_command_arr;
		if(strstr(cmd,"|")!=NULL)
		{
			piped_command_arr = create_argv(cmd,"|",1);
			if(piped_command_arr == NULL){
				SAFE_PRINTER(STDERR_FILENO, "syntax error near unexpected token `|'\n");
				child_daycare(0);
				job_report(SYNC);
				free(cmd_reset);
				goto w_uber;
			}
		}
		else
		{
			piped_command_arr = create_argv(cmd,"|",1);
			if(piped_command_arr == NULL){
				child_daycare(0);
				job_report(SYNC);
				free(cmd_reset);
				goto w_uber;
			}
		}


		if(piped_command_arr[0]==NULL){
			free(cmd_reset);
			free(piped_command_arr);
			goto w_uber;
		}


		//get the pipes setup
		int numpipes=0;
		for(int i = 0; piped_command_arr[i] != NULL; i++){
			numpipes++;
		}
		int fds_size = numpipes*2;
		int pipefds[fds_size];

		for(int i = 0; i < fds_size; i+=2){
			pipe(pipefds+i);
		}
		
		pid_t CUR_GPID = -1;
		process*proc;

		int was_forked=1;

		//if we have to pipe, get ready for that
		if(numpipes>0){
			CURRENT_JOB = malloc(sizeof(job));
			CURRENT_JOB->cmd = malloc((strlen(cmd_reset)+1)*sizeof(char));
			strcpy(CURRENT_JOB->cmd,cmd_reset);
			CURRENT_JOB->procs_head = NULL;
			CURRENT_JOB->pgid = -1;
			CURRENT_JOB->cur_status = TERMINATED;
			CURRENT_JOB->old_status = -1;
			CURRENT_JOB->latest_info = 0;
			
			time_t t;
			time(&t);
			struct tm * start_time = localtime(&t);
			CURRENT_JOB->start_time = malloc(10*sizeof(char));
			sprintf(CURRENT_JOB->start_time,"%d:%d",start_time->tm_hour,start_time->tm_min);
			
			for(int i = 0; i < numpipes; i++){
				process * proc = malloc(sizeof(process));
				proc->pid = -1;
				proc->status = RUNNING;
				add_process(CURRENT_JOB,proc);
			}
			proc = CURRENT_JOB->procs_head;
		}

		//multi part command loop
		for(int i = 0; piped_command_arr[i] != NULL; i++){
			was_forked=1;
			strcpy(cmd,piped_command_arr[i]);
			//char * cmd_reset = malloc((strlen(cmd)+1)*sizeof(char));
			strcpy(cmd_reset,cmd);
			
			//redirection
			int fd_out = STDOUT_FILENO;
			int fd_in = STDIN_FILENO;	
			int fd_errout = STDERR_FILENO;
			
			char* pipe_start=cmd_reset+(strlen(cmd_reset));
			char *temp=NULL;



			//ALL REDIRECTION CASES
			
			if(temp=strstr(cmd_reset,"2>"), temp != NULL){
				if(temp<pipe_start)
					pipe_start=temp;
				temp[1]=' ';
				temp = temp+2;
				char* file = get_next_token(&temp," ");
				fd_errout = open(file, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
			}
			if(temp=strstr(cmd_reset,">"), temp != NULL){
				if(temp<pipe_start)
					pipe_start=temp;
				temp = temp+1;
				char* file = get_next_token(&temp," ");
				fd_out = open(file, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
			}
			if(temp=strstr(cmd_reset,"<"), temp != NULL){
				if(temp<pipe_start)
					pipe_start=temp;
				temp = temp+1;
				char* file = get_next_token(&temp," ");
				fd_in = open(file, O_RDWR, S_IRUSR | S_IWUSR);
				if(fd_in==-1){
					SAFE_PRINTER(curr_fd,FILE_NOT_FOUND);
					goto w_uber;
				}
			}
			


			pipe_start[0]='\0';
			strcpy(cmd,cmd_reset);
			char*command = get_next_token(&cmd, " ");
			builtin_pair bp;

			//BUILT INS
			if(bp = get_bltin(command),bp.bltin != NULL){
				//IF WE CANT FORK
				if(bp.FORKABLE==0){
					was_forked=0;
					if(bp.bltin_func == sf_exit){
						for(int i = 0; i < fds_size; i++)
							close(pipefds[i]);
						append_job(CURRENT_JOB);
					}
					retvalue = bp.bltin_func(cmd);
					if(CUR_GPID==-1)
						CUR_GPID = getpid();

				}else{//IF WE CAN FORK

					int childpid = fork();
					if(CUR_GPID==-1)
						CUR_GPID = childpid;
					proc->pid = childpid;
					if(childpid == 0){
						//SAVE THE CHILDREN
						setpgid(getpid(),CUR_GPID);
						signal(SIGTSTP,SIG_DFL);
						signal(SIGINT,SIG_DFL);
						signal(SIGCHLD,SIG_DFL);
						signal(SIGPIPE,SIG_DFL);

						if(piped_command_arr[i+1]!=NULL){
							dup2(pipefds[i*2+1],1);
						}if(i!=0){
							dup2(pipefds[i*2-2],0);
						}for(int l = 0; l < fds_size; l++){
							close(pipefds[l]);
						}
						dup2(fd_in,STDIN_FILENO);
						dup2(fd_out,STDOUT_FILENO);
						dup2(fd_errout,STDERR_FILENO);
						
						dispose = bp.bltin_func(cmd);
						sf_exit(NULL);
					}else{
					}
				}
			}//PROGRAMS
			else{
				//FORK AND REDIRECT OUTPUT
				//ALSO FIX THE SIGNAL HANDLERS
				
				int childpid = fork();
				if(CUR_GPID==-1)
					CUR_GPID = childpid;
				proc->pid = childpid;
				if(childpid == 0){
					setpgid(getpid(),CUR_GPID);
					if(!bg_flag)
						tcsetpgrp(0,getpgrp());
					signal(SIGTSTP,SIG_DFL);
					signal(SIGINT,SIG_DFL);
					signal(SIGCHLD,SIG_DFL);
					signal(SIGPIPE,SIG_DFL);
					if(command[0]=='/' || (command[0]=='.' && command[1]=='/')){
						char ** cuck = create_argv(cmd_reset, " ",0);
						execvp(command,cuck);
						exit(EXIT_SUCCESS);
					}else{
						char* prg = find_program(command);
						if(prg != NULL){
							if(piped_command_arr[i+1]!=NULL){
								dup2(pipefds[i*2+1],1);
							}if(i!=0){
								dup2(pipefds[i*2-2],0);
							}for(int l = 0; l < fds_size; l++){
								close(pipefds[l]);
							}
							dup2(fd_in,STDIN_FILENO);
							dup2(fd_out,STDOUT_FILENO);
							dup2(fd_errout,STDERR_FILENO);
							char ** cuck = create_argv(cmd_reset, " ",0);
							
							execvp(prg,cuck);
							exit(EXIT_SUCCESS);
						}else{
							SAFE_PRINTER(curr_fd,COMMAND_NOT_FOUND);
							exit(1);
						}
					}
				}else{
					char* prg = find_program(command);
					if(prg==NULL){
						NUM_COMMANDS--;
						retvalue = 1;
					}
					free(prg);
					setpgid(childpid,CUR_GPID);
				}
			}
			if(!was_forked){
				proc->status=TERMINATED;
			}
			proc=proc->next;
		}

		CURRENT_JOB->pgid = CUR_GPID;
		append_job(CURRENT_JOB);
		NUM_COMMANDS++;


		for(int i = 0; i < fds_size; i++)
			close(pipefds[i]);
		if(!bg_flag){
			tcsetpgrp(0,CUR_GPID);
			//LOOP FOR JOB IN FG
			int info;
			pid_t pid;

			if(!stop_wait(CURRENT_JOB)){
				while(pid = waitpid(-1, &info, WUNTRACED), !change_states(pid, info) && !stop_wait(CURRENT_JOB));
				//LOOP FOR JOB IN FG
				tcsetpgrp(0,getpgrp());
			}
		}

		//PRINT OUTPUT IF IT WASNT PRINTED
		child_daycare(-1);
		job_report(SYNC);
		free(cmd_reset_keeper);
		for(int i = 0; piped_command_arr[i] != NULL; i++){
			free(piped_command_arr[i]);
		}
		free(piped_command_arr);
	}

    free(cmd);
    cmd = NULL;
    return EXIT_SUCCESS;
}

//list all current jobs
int sf_jobs(char*args){
		job*c = joblist_head;
		char*RUNNING_str = "Running";
		char*STOPPED_str = "Stopped";

		job_status_changer();

		while(c != NULL){
			if(c->cur_status!=TERMINATED){
				char*status = NULL;
				if(c->cur_status==STOPPED)
					status = STOPPED_str;
				else
					status = RUNNING_str;

				char a[50]; sprintf(a,"[%d]  %s  %d  %s\n",c->jobid,status,c->pgid,c->cmd);
				SAFE_PRINTER(STDOUT_FILENO,a);
			}

			c = c->next;
		}
		SAFE_PRINTER(STDOUT_FILENO,"\n");

		return 0;
}

//SEND A SIGNAL TO A PROCESS
int sf_kill(char*args){
	char*num1 = get_next_token(&args," ");
	char*num2 = get_next_token(&args," ");
	int signal = SIGTERM;
	int id_num;

	if(num1 == NULL)
		return 1;

	job* cur = joblist_head;
	int isjobid=0;

	if(num2 == NULL){
		if(num1[0]=='%'){
			isjobid = 1;
			id_num = strtol(num1+1,NULL,10);
		}else
			id_num = strtol(num1,NULL,10);
	}else{
		signal = strtol(num1,NULL,10);
		if(num2[0]=='%'){
			isjobid = 1;
			id_num = strtol(num2+1,NULL,10);
		}
		else
			id_num = strtol(num2,NULL,10);
	}

	if(signal < 1 || signal > 31){
		SAFE_PRINTER(STDERR_FILENO,"invalid signal.\n");
		return 1;
	}

	while(cur != NULL){
		if((isjobid && cur->jobid == id_num)||(isjobid==0 && cur->pgid == id_num)){
			killpg(cur->pgid,signal);
			return 0;
		}
		cur=cur->next;
	}
	if(num1[0]=='%')
		SAFE_PRINTER(STDERR_FILENO,num1,": no such job\n");
	else
		SAFE_PRINTER(STDERR_FILENO,num1,": no such pid\n");
	return 1;
}

//bring bg process to fg
int sf_fg(char*args){
	char*num1 = get_next_token(&args," ");
	int id_num;

	job* cur = joblist_head;
	int isjobid=0;

	if(num1 == NULL){
		SAFE_PRINTER(STDERR_FILENO,"ERROR: must specify an id\n");
		return 1;
	}

	if(num1[0]=='%'){
		isjobid = 1;
		id_num = strtol(num1+1,NULL,10);
	}else{
		id_num = strtol(num1,NULL,10);
	}


	while(cur != NULL){
		if((isjobid==0 && cur->pgid == id_num) || (isjobid && cur->jobid == id_num)){

			if(cur->cur_status == STOPPED){
	     			killpg(cur->pgid,SIGCONT);
			}else if(cur->cur_status == TERMINATED){
				break;
			}

			int info;
			pid_t pid;

			child_daycare(0);

			//WHAT IF ONE IS DEAD?
			while(tcsetpgrp(0,cur->pgid));
			
			if(!stop_wait(cur)){
				while(pid = waitpid(-1, &info, WUNTRACED), !change_states(pid, info) && !stop_wait(cur));
			}
			//LOOP FOR JOB IN FG
			//
			tcsetpgrp(0,getpgrp());

			return 0;
		}
		cur=cur->next;
	}
	SAFE_PRINTER(STDERR_FILENO,num1,": no such job\n");
	return 1;
}


//send the fg process to the bg
int sf_bg(char*args){
	char*num1 = get_next_token(&args," ");
	int id_num;

	job* cur = joblist_head;
	int isjobid;

	if(num1 == NULL){
		SAFE_PRINTER(STDERR_FILENO,"ERROR: must specify an id\n");
		return 1;
	}

	if(num1[0]=='%'){
		isjobid = 1;
		id_num = strtol(num1+1,NULL,10);
	}else{
		id_num = strtol(num1,NULL,10);
	}

	while(cur != NULL){
		if((isjobid && cur->jobid == id_num)||(!isjobid && cur->pgid == id_num)){
			if(cur->procs_head->status == RUNNING){
				SAFE_PRINTER(STDERR_FILENO,"process ",num1,": already running\n");
				return 1;
			}
			for(process* cur_p = cur->procs_head; cur_p; cur_p = cur_p->next)
				cur_p->status = RUNNING;
				
			killpg(cur->pgid,SIGCONT);
			return 0;
		}
		cur=cur->next;
	}
	SAFE_PRINTER(STDERR_FILENO,num1,": no such job\n");
	return 1;
}


//detach the fg process
int sf_disown(char*args){
	char*num1 = get_next_token(&args," ");
	int id_num;
	job* cur = joblist_head;
	int allflag = 0;
	int isjobid=0;

	if(num1 == NULL){
			allflag = 1;
			num1++;
	}else{
		if(num1[0]=='%'){
			id_num = strtol(num1+1,NULL,10);
			isjobid=1;
		}else
			id_num = strtol(num1,NULL,10);
	}
	while(cur != NULL){
		if(allflag){
			job*next=cur->next;
			remove_job(cur);
			cur = next;
			if(cur==NULL)
				return 0;
		}else{
			if(isjobid && cur->jobid == id_num){
				remove_job(cur);
				return 0;
			}if(!isjobid && cur->pgid == id_num){
				remove_job(cur);
				return 0;
			}
			cur=cur->next;
		}
	}

	if(allflag)
		return 0;

	if(num1[0]=='%')
		SAFE_PRINTER(STDERR_FILENO,num1,": no such job\n");
	else
		SAFE_PRINTER(STDERR_FILENO,num1,": no such pid\n");

	return 1;
}

//Print a list of all builtin’s and their basic usage in a single column. Type help in bash to get an idea.
int sf_help(char*args){
	for(int k = 0; k < builtin_pairs_size; k++){
		SAFE_PRINTER(curr_fd,builtin_pairs[k].usage);
	}
	return 0;
}

//Exits the shell by using the exit(3) function.
int sf_exit(char*args){
	for(job*j=joblist_head;j!=NULL;){
		job*t = j->next;
		remove_job(j);
		j=t;
	}
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);

	free(prompt_str);
	free(current_working_directory);
	exit(dispose);
	return 0;
}

//Changes the current working directory of the shell by using the chdir(2) system call.
int sf_cd(char * args){
	//strip the string
	char*proper_path = get_next_token(&args," ");

	if(proper_path != NULL){
		int k;
		if(strcmp(proper_path,"-")==0){
			k = chdir(getenv("OLDPWD"));
			setenv("OLDPWD",current_working_directory,1);
		} else{
			k = chdir(proper_path);
			if(k == 0)
				setenv("OLDPWD",current_working_directory,1);
		}

		prompt_update(WORKING_DIR_CHANGE); 
		return k;
	}else{
		int k = chdir(getenv("HOME"));
		if(k == 0)
			setenv("OLDPWD",current_working_directory,1);
		prompt_update(WORKING_DIR_CHANGE); 
		return k;
	}
	return 1;
}

//Prints the absolute path of the current working directory which can be obtained through the getcwd(3) function.
int sf_pwd(char*args){
	curr_dir_update();
	SAFE_PRINTER(curr_fd,current_working_directory,"\n");
	return 0;
}


//Prints the return code of the command that was last executed.
int sf_prt(char* args){
	char k = '0';
	if(retvalue)
		k='1';

	char p[4] = {k,'\n','\0'};

	SAFE_PRINTER(curr_fd,p);
	return 0;
}

//Change prompt settings.
int sf_chpmt(char*args){
	char *SETTING = get_next_token(&args," ");
	char *TOGGLE = get_next_token(&args," ");
	int toggle_num;
	if(SETTING == NULL || TOGGLE == NULL){
		SAFE_PRINTER(STDERR_FILENO,"INVALID ARGUMENTS.\n");
		return 1;
	}
	toggle_num = strtol(TOGGLE, NULL, 10);
	if(strlen(TOGGLE) != 1 || !(toggle_num==1 || toggle_num==0)){
		SAFE_PRINTER(STDERR_FILENO,"INVALID ARGUMENTS.\n");
		return 1; 
	}

	if(strcmp(SETTING,"user")==0){
		USER_PROMPT.user = toggle_num;
	}else if(strcmp(SETTING,"machine")==0){
		USER_PROMPT.machine = toggle_num;
	}else{
		SAFE_PRINTER(STDERR_FILENO,"INVALID ARGUMENTS.\n");
		return 1;
	}

	prompt_update(INFO_CHANGE);
	return 0;
}

//Change prompt colors.
int sf_chclr(char*args){
	char*SETTING = get_next_token(&args, " ");
	char*COLOR = get_next_token(&args, " ");
	char*BOLD = get_next_token(&args, " ");

	if(BOLD==NULL){
		SAFE_PRINTER(STDERR_FILENO,"INVALID ARGUMENTS.\n");
		return 1;
	}

	int bold, clr=-1, i = 0;
	for(; i < ansi_colors_size; i++){
		if(strcmp(ansi_colors[i].color,COLOR)==0){
			clr = i;
			break;
		}
	}
	if(clr == -1){
		SAFE_PRINTER(STDERR_FILENO,"INVALID ARGUMENTS.\n");
		return 1;
	}

	bold = strtol(BOLD, NULL, 10);
	if(!(bold==1 || bold==0)){
		SAFE_PRINTER(STDERR_FILENO,"INVALID ARGUMENTS.\n");
		return 1;
	}

	if(strcmp(SETTING,"user")==0){
		USER_PROMPT.user_color = clr;
		USER_PROMPT.user_bold = bold;
	}else if(strcmp(SETTING,"machine")==0){
		USER_PROMPT.machine_color = clr;
		USER_PROMPT.machine_bold = bold;
	}else{
		SAFE_PRINTER(STDERR_FILENO,"INVALID ARGUMENTS.\n");
		return 1;
	}
	
	prompt_update(INFO_CHANGE);
	return 0;
}	






//HELPER FUNCTIONS =========================================================================
builtin_pair get_bltin(char*cmd){
	for(int i = 0; i < builtin_pairs_size; i++){
		if(strcmp(cmd,builtin_pairs[i].bltin)==0)
			return builtin_pairs[i];
	}
	return builtin_pairs[builtin_pairs_size];
}

char * find_program(char*program_name){
	char* pure_path = malloc((strlen(getenv("PATH"))+1)*sizeof(char));
	char* pp = pure_path;
	strcpy(pure_path,getenv("PATH"));
	char*ret_path = malloc(sizeof(char)*(strlen(pure_path)+1));
	struct stat buffer;
	char* token = NULL;
	
	while(pure_path!=NULL){
		token = get_next_token(&pure_path,":");
		strcpy(ret_path,token);
		strcat(ret_path,"/");
		strcat(ret_path,program_name);
		if(stat(ret_path,&buffer)==0){
			free(pp);
			return ret_path;
		}
	}
	free(pp);
	free(ret_path);
	return NULL;
}

char** create_argv(char*args,char* delim, int RIGID){
	if(RIGID && args[0]=='|')
		return NULL;
	size_t arg_array_size = 10;
	int i = 0;
	char** arg_array;


	arg_array = malloc(arg_array_size*sizeof(char*));
	char* token = NULL;

	for(; args!=NULL; i++){
		if(i == arg_array_size-1){
			arg_array = realloc(arg_array,arg_array_size*2);
			arg_array_size*=2;
		}

		if(RIGID)
			token = strsep(&args,delim);
		else
			token = get_next_token(&args,delim);

		if(token == NULL && RIGID)
			return NULL;
		if(token==NULL)
			break;

		while(token[0]==' ')
			token++;
		if(token[0]=='\0')
		{
			if(i!=0)
				return NULL;
			i--;
		}
		else{
			arg_array[i] = malloc(strlen(token)+1);
			strcpy(arg_array[i],token);
			//arg_array[i] = token;
		}
	}

	arg_array[i] = NULL;
	if(arg_array[0]==NULL)
		return NULL;
	return arg_array;
}


char* get_next_token(char**str_to_tok,char* delim){
	if(*str_to_tok == NULL)
			return NULL;
	char*token; 
	for(token = strsep(str_to_tok,delim); *str_to_tok != NULL && token[0] == '\0';){
		token = strsep(str_to_tok,delim);
	}
	if(token[0] == '\0')
		token = NULL;

	return token;
}

char* contains_at_end(char*str,char c){
	size_t c_size = strlen(str);
	char*cur = ((void*)str) + c_size-1;
	while(cur > str){
		if(*cur == c){
			return cur;
		}else if(*cur != ' ')
			return NULL;

		cur--;
	}
	return NULL;
}

void safe_print(int fd,char * str, ...){
	va_list str_list;
	
	va_start(str_list,str);
	write(fd,str,strlen(str));
	
	for(char*k = va_arg(str_list,char*); k!=NULL; k=va_arg(str_list,char*)){
		write(fd,k,strlen(k));
	}
}

void curr_dir_update(){
	if(current_working_directory == NULL)
		current_working_directory = malloc((cwd_size+1)*sizeof(char));

	getcwd(current_working_directory, cwd_size);

	while(errno == ERANGE){
		cwd_size *=2;
		current_working_directory = realloc(current_working_directory, cwd_size);
		errno = 0;
		getcwd(current_working_directory, cwd_size);
	}
}

void prompt_update(PROMPT_CHANGE_TYPE t){
	if(prompt_str == NULL)
		prompt_str = malloc(prompt_str_size);
	
	if(t == WORKING_DIR_CHANGE || t == FULL_CHANGE || current_working_directory == NULL)
		curr_dir_update();

	char *bldr[12], *sfish="sfish", *user=getenv("USER"), *colon=":[", *dash="-", *at="@", machine[64], *arrow="]> " ;
	gethostname(machine, 64);
	int index = 0;


	bldr[index++] = sfish;
	if(USER_PROMPT.user || USER_PROMPT.machine) bldr[index++]=dash;


	if(USER_PROMPT.user){
		if(USER_PROMPT.user_color != -1){
			if(USER_PROMPT.user_bold==0)
				bldr[index++]=ansi_colors[USER_PROMPT.user_color].esc_seq;
			else
				bldr[index++]=ansi_colors[USER_PROMPT.user_color].esc_seq_bold;
		}
	     	bldr[index++]=user;
		if(USER_PROMPT.user_color != -1){
	     		bldr[index++]= "\001\033[0m\002";
		}
	}
	if(USER_PROMPT.machine && USER_PROMPT.user) bldr[index++]=at;
	if(USER_PROMPT.machine){
		if(USER_PROMPT.machine_color != -1){
			if(USER_PROMPT.machine_bold==0)
				bldr[index++]=ansi_colors[USER_PROMPT.machine_color].esc_seq;
			else
				bldr[index++]=ansi_colors[USER_PROMPT.machine_color].esc_seq_bold;
		}
	     	bldr[index++]=machine;
		if(USER_PROMPT.machine_color != -1){
	     		bldr[index++]= "\001\033[0m\002";
		}
	}

	bldr[index++] = colon;


	//TILDA SUPPORT
	char * home = getenv("HOME");
	char * mod_cwd = NULL;	
	if(strncmp(home,current_working_directory,strlen(home))==0){
		mod_cwd = malloc(sizeof(char)*strlen(current_working_directory+1));
		mod_cwd[0]='\0';
		strcat(mod_cwd,"~");
		strcat(mod_cwd,current_working_directory+strlen(home));
	}else{
		mod_cwd = current_working_directory;
	}
			
	bldr[index++] = mod_cwd;
	bldr[index++] = arrow;

	prompt_str[0] = '\0';
	for(int i = 0; i < index; i++){
		strcat(prompt_str,bldr[i]);
	}

	if( mod_cwd!= current_working_directory)
		free(mod_cwd);
}

//JOBS===============================================================
job* append_job(job*job_to_add){
	if(joblist_head==NULL){
		job_to_add->jobid=1;
		joblist_head=job_to_add;
		job_to_add->next = NULL;
	}else {
		job* temp = joblist_head;
		int i = 1;
		if(temp->jobid > i++){
			job_to_add->jobid = 1;
			joblist_head = job_to_add;
			job_to_add->next= temp;
		}else{
			while(temp != NULL){
				job* temp_next = temp->next;
				if(temp_next==NULL){
					job_to_add->jobid = i;
					insert_job(temp,job_to_add,NULL);
					break;
				}
				if(i < temp_next->jobid){
					job_to_add->jobid = i;
					insert_job(temp,job_to_add,temp_next);
					break;
				}
				temp= temp->next;
				i++;
			}
		}
	}
	return job_to_add;
}
void insert_job(job*prev, job*insert, job*next){
	prev->next = insert;
	insert->next = next;
}
 
void remove_job(job*j){
	if(j == SPID) SPID = NULL;
	if(joblist_head == j){
		joblist_head=j->next;
	}else{
		for(job*cur=joblist_head;cur!=NULL;cur=cur->next){
			if(cur->next == j){
				cur->next = j->next;
			}
		}
	}

	process*next = j->procs_head;
	while(next != NULL){
		process* temp = next->next;
		free(next);
		next = temp;
	}
	
	free(j->cmd);
	free(j->start_time);
	free(j);
}

void add_process(job * j, process * proc){
	proc->next = j->procs_head;
	j->procs_head = proc;
}

int change_states(pid_t pid, int info){
	for(job* cj = joblist_head; cj!=NULL; cj = cj->next){
		for(process*proc = cj->procs_head; proc!=NULL; proc = proc->next){
			if(proc->pid == pid){
				proc->info = info;
				cj->latest_info = info;
				if(WIFCONTINUED(proc->info)){
					proc->status = RUNNING;
				}else if(WIFSTOPPED(proc->info)){
					proc->status = STOPPED;
				}else{//terminated
					proc->status = TERMINATED;
					SPID = NULL;
				}
				return 0;
			}
		}
	}
	return 1;
}


int stop_wait(job * j){
	for(process * proc = j->procs_head; proc != NULL; proc = proc->next){
		if(!(proc->status==TERMINATED || proc->status==STOPPED)){
			return 0;
		}
	}
	return 1;
}

void job_status_changer(){
	for(job * cur_job = joblist_head; cur_job!=NULL; cur_job=cur_job->next){
		int finished=1, stopped=1, running=1;
		for(process * proc = cur_job->procs_head; proc!= NULL; proc = proc->next){
			finished=(finished && proc->status == TERMINATED);
			stopped=(stopped && (proc->status==TERMINATED || proc->status==STOPPED));
			running=(running && (proc->status==TERMINATED || proc->status==RUNNING));
		}
		int valid = finished|stopped|running;
		if(valid){
			cur_job->old_status = cur_job->cur_status;
			if(finished){
				cur_job->cur_status = TERMINATED;
				retvalue = WEXITSTATUS(cur_job->latest_info);
			}else if(stopped){
				cur_job->cur_status = STOPPED;
			}else if(running){
				cur_job->cur_status = RUNNING;
			}
		}
	}
}

void job_report(PRINTER p_t){
	char a[256]; 

	job_status_changer();

	job * cj = joblist_head;
	while(cj!=NULL){
		if(cj->cur_status!=cj->old_status){			
			if(cj->cur_status==TERMINATED){
				if(WIFSIGNALED(cj->latest_info)){
					sprintf(a,"[%d]  %d  Terminated  %s by signal %d\n",cj->jobid,cj->pgid,cj->cmd,WTERMSIG(cj->latest_info));
				}else{
					sprintf(a,"[%d]  %d  Completed  %s with status %d\n",cj->jobid,cj->pgid,cj->cmd,WEXITSTATUS(cj->latest_info));
				}
				if(p_t == ASYNC) SAFE_PRINTER(STDOUT_FILENO,"\n");
				SAFE_PRINTER(STDOUT_FILENO,a);
				if(p_t == ASYNC) rl_forced_update_display();//
				job*temp = cj;
				cj=cj->next;
				remove_job(temp);
			}else if(cj->cur_status==STOPPED){
				if(WIFSTOPPED(cj->latest_info)){
					sprintf(a,"[%d]  %d  Stopped  %s by signal %d\n",cj->jobid,cj->pgid,cj->cmd,WSTOPSIG(cj->latest_info));
					if(p_t == ASYNC) SAFE_PRINTER(STDOUT_FILENO,"\n");
					SAFE_PRINTER(STDOUT_FILENO,a);
					if(p_t == ASYNC) rl_forced_update_display();//
				}
				cj=cj->next;
			}else{
				if(WIFCONTINUED(cj->latest_info)){
					sprintf(a,"[%d]  %d  Continued  %s by signal %d\n",cj->jobid,cj->pgid,cj->cmd,SIGCONT);
				}else{
					sprintf(a,"[%d]  %d  Running  %s\n",cj->jobid,cj->pgid,cj->cmd);
				}
				if(p_t == ASYNC) SAFE_PRINTER(STDOUT_FILENO,"\n");
				SAFE_PRINTER(STDOUT_FILENO,a);
				if(p_t == ASYNC) rl_forced_update_display();//
				cj=cj->next;
			}
		}else{
			if(cj->cur_status==TERMINATED){
				//retvalue = WEXITSTATUS(cj->latest_info);
				job*temp = cj;
				cj=cj->next;
				remove_job(temp);
			}else
				cj=cj->next;
		}
	}
}


//HANDLERS========================================================
void child_daycare(int sig){
	sigprocmask(SIG_BLOCK,&mask,NULL);

	int info; pid_t pid;
	while(pid = waitpid(-1, &info, WUNTRACED|WCONTINUED|WNOHANG), pid > 0 && change_states(pid, info));

	if(sig != SIGCHLD)
		job_report(SYNC);
	else
		job_report(ASYNC);

	sigprocmask(SIG_UNBLOCK,&mask,NULL);
}

int help_handler(int count, int key){
	SAFE_PRINTER(STDOUT_FILENO,"\n");
	sf_help(NULL);
	rl_forced_update_display();
	return 1;
}

int store_pid_handler(int count, int key){
	SPID = joblist_head;
	return 1;
}

int get_pid_handler(int count, int key){
	if(SPID != NULL){
		SAFE_PRINTER(STDOUT_FILENO,"\n");
		killpg(SPID->pgid, SIGTERM);
	}else
		write(STDOUT_FILENO,SPID_NOT_FOUND,strlen(SPID_NOT_FOUND));

	rl_forced_update_display();
	return 1;
}

int sfish_info_handler(int count, int key){
	SAFE_PRINTER(STDOUT_FILENO, "\n---BUILTINS---");
	for(int i = 0; i < builtin_pairs_size; i++){
		SAFE_PRINTER(STDOUT_FILENO, "\n", builtin_pairs[i].bltin);
	}
	SAFE_PRINTER(STDOUT_FILENO, "\n---NUMBER OF COMMANDS RAN---\n");
	char a[24];
	sprintf(a,"%li",NUM_COMMANDS);
	SAFE_PRINTER(STDOUT_FILENO,a,"\n");

	SAFE_PRINTER(STDOUT_FILENO,"---PROCESS TABLE---\nPGID\tPID\tTIME\tCMD\n");
	for(job*cj=joblist_head;cj!=NULL;cj=cj->next){
		sprintf(a,"%d",cj->pgid);
		SAFE_PRINTER(STDOUT_FILENO,a,"\t",a,"\t",cj->start_time,"\t",cj->cmd,"\n");
	}
	
	rl_forced_update_display();
	return 1;
}
