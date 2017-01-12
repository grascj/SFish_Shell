#include <stdarg.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <wait.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <readline/readline.h>
#include <readline/history.h>


typedef int (*proc_pointer)(char*);


//struct for builtin data
typedef struct builtin_pair {
	char * bltin;
	char * usage;
	proc_pointer bltin_func;
	int FORKABLE;
} builtin_pair;

//status process
typedef enum {RUNNING, TERMINATED, STOPPED, PRINTED, NOT_PRINTED} PROC_STATUS;

//process data
typedef struct process{
	struct process *next;
	pid_t pid;
	PROC_STATUS status;
	int info;
} process;


//struct for job data
typedef struct job{
	process *procs_head;
	char *cmd;
	int jobid;
	PROC_STATUS cur_status;
	PROC_STATUS old_status;
	int continued;
	pid_t pgid;
	int latest_info;
	struct job *next;
	char* start_time;
} job;

//color struct
typedef struct ansi_color {
	char* color;
	char* esc_seq;
	char* esc_seq_bold;
} ansi_color;

char ansi_colors_size = 8;
ansi_color ansi_colors[] = {
	{"black",	"\001\x1B[30m\002",	"\001\x1B[1;30m\002"},
	{"red",	"\001\x1B[31m\002",	"\001\x1B[1;31m\002"},
	{"green",	"\001\x1B[32m\002",	"\001\x1B[1;32m\002"},
	{"yellow",	"\001\x1B[33m\002",	"\001\x1B[1;33m\002"},
	{"blue",	"\001\x1B[34m\002",	"\001\x1B[1;34m\002"},
	{"magenta",	"\001\x1B[35m\002",	"\001\x1B[1;35m\002"},
	{"cyan",	"\001\x1B[36m\002",	"\001\x1B[1;36m\002"},
	{"white",	"\001\x1B[37m\002",	"\001\x1B[1;37m\002"}
};

//error messages
#define COMMAND_NOT_FOUND "command not found.\n"
#define FILE_NOT_FOUND "file not found.\n"
#define SPID_NOT_FOUND "\nSPID does not exist and has been set to -1\n"

//prompt changes
typedef enum {FULL_CHANGE, INFO_CHANGE, WORKING_DIR_CHANGE} PROMPT_CHANGE_TYPE;

//struct for the user prompt
typedef struct prompt {
	//1 if used
	int user;
	int machine;
	//index in ansi_colors[]
	int user_color;
	int machine_color;
	//1 if bold
	int user_bold;
	int machine_bold;
} prompt;


//HELPER FUNCTIONS ===========================================================

/* get the builtin for the command */
builtin_pair get_bltin(char*cmd);

/* add and run the process */
void add_process(job * j, process * proc);

/* get the last non-blank character */
char * contains_at_end(char*,char);

/* match the string with the program */
char *find_program(char*program_name);

/* split the string up by the delimiter into an array of strings */
char **create_argv(char*args,char*delim,int RIGID);

/* update the current directory */
void curr_dir_update();

/* thread safe print */
void safe_print(int fd,char*str,...);

/* get_next_token */
char *get_next_token(char**str_to_tok,char*delim);

/* update the prompt */
void prompt_update(PROMPT_CHANGE_TYPE);

//JOBS =======================================================================

/* job control */
int stop_wait(job*);
void insert_job(job*prev, job*insert, job*next);
void remove_job(job*to_remove);
job* append_job(job*job_to_add);
int change_states(pid_t pid, int info);
void job_report();
void job_status_changer();

//HANDLERS ==================================================================

//keypress handlers
int get_pid_handler(int count, int key);
int sfish_info_handler(int count, int key);
int store_pid_handler(int count, int key);
int help_handler(int count, int key);

//handle child process dying
void child_daycare(int sig);

//BUILTINS ==================================================================


/* list current jobs */
int sf_jobs(char*args);

/* send a signal to a PID */
int sf_kill(char*args);

/* disown a job */
int sf_disown(char*args);

/* send job to background */
int sf_bg(char*args);

/* bring job to foreground */
int sf_fg(char*args);

/* bring up usage */
int sf_help(char * args);

/* exit and kill child processes */
int sf_exit(char * args);

/* change directory */
int sf_cd(char * args);

/* print working directory */
int sf_pwd(char * args);

/* print last exit status */
int sf_prt(char * args);

/* change prompt */
int sf_chpmt(char * args);

/* change prompt color */
int sf_chclr(char * args);


//BUILTINS====================================================================
const static builtin_pair builtin_pairs[]={
	{"chclr",	"chclr SETTING COLOR BOLD   change prompt color\n",	&sf_chclr,		0},
	{"cd",	"cd PATH                    change directory\n",				&sf_cd,		0},
	{"chpmt",	"chpmt SETTING TOGGLE       change sfish prompt\n",		&sf_chpmt,		0},
	{"exit",	"exit                       exit sfish\n",					&sf_exit,		0},
	{"help",	"help                       print help menu\n",					&sf_help,		1},
	{"prt",	"prt                        print exit status\n",					&sf_prt,		1},
	{"pwd",	"pwd                        print working directory\n",					&sf_pwd,		1},
	{"kill",	"kill [SIGNAL] PID|JID      send a signal to a process\n",		&sf_kill,		0},//8
	{"jobs",	"jobs                       print all current background jobs\n",					&sf_jobs,		1},//9
	{"disown",	"disown [PID|JID]           disown a job\n",				&sf_disown,		0},//10
	{"bg",	"bg PID|JID                 continue a background process\n",					&sf_bg,		0},//11
	{"fg",	"fg PID|JID                 bring a background process to the foreground\n",					&sf_fg,		0},//12

	{NULL,	NULL,						NULL,			-1}
};
