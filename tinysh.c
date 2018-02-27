// Author: Khuong Luu


#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/limits.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
typedef enum {false, true} bool;

// global vairiables
int g_last_exit_code;
int g_last_terminate_signal_code;
int g_was_terminated;
char g_working_directory[100];
bool g_is_bg_command = false;
pid_t g_background_pids[200];
int g_num_background_pids = 0;
bool g_is_child_proc = false;
bool g_is_bg_proc = false;
bool g_bg_command_enable = true;
bool g_signal_caught = false;
int g_file_src;
int g_file_dest;
char* g_file_src_path = NULL;
char* g_file_dest_path = NULL;

// structs
struct sigaction g_sigint_action = {0};
struct sigaction g_sigterm_action = {0};
struct sigaction g_sigtstp_action = {0};
sigset_t g_blocked_signal_set;

// constants
const int MAX_INPUT_LENGTH = 2049;
const int MAX_ARG_LENGTH = 50;
const int MAX_NUM_ARG = 512;
const int MAX_NUM_TOKENS = 50;
const int MAX_PATH_LENGTH = 100;
const int MAX_TOKEN_LENGTH = 50;
const int MAX_BG_PID = 200;
const int FORK_FAILED_ERROR_CODE = 1;
const int EXECUTE_FAILED_ERROR_CODE = 1;
const int WAIT_FAILED_ERROR_CODE = 1;
const int DEFAULT_BG_PID = -3;
const int DEFAULT_NEG_INT = -5;
const int NO_REDIRECTION = -1;
const int NOT_FOUND = -1;


// Signal //

// NOTE: strcpy and strcat is async-safe 
void PrintMessage(const char* message) {
	char buffer[50];
	memset(buffer, '\0', sizeof(buffer));
	strcpy(buffer, message);
	strcat(buffer, "\n\0");
	write(STDOUT_FILENO, buffer, 50);
}

// Child
void child_catch_sigint(int signo) {
	PrintMessage("CHILD caught SIGINT");
	if (g_is_bg_proc) {
		// do nothing
		PrintMessage("bg CHILD caught SIGINT");
		return;	
	}
	exit(SIGINT);
}

void child_catch_sigterm(int signo) {
	PrintMessage("CHILD caught SIGNTERM");
	if (g_is_bg_proc) {
		// do nothing
		PrintMessage("bg CHILD caught SIGINT");
		return;	
	}
	exit(SIGTERM);
}

// Parent
void parent_catch_sigint(int signo) {
	PrintMessage("PARENT caught SIGINT");
	g_signal_caught = true;	
	//kill(-1, SIGINT); // set SIGNINT to all childs
}

void parent_catch_sigterm(int signo) {
	PrintMessage("PARENT caught SIGTERM");
	g_signal_caught = true;	
	kill(-1, SIGTERM); // set SIGTERM to all childs
	PrintMessage("PARENT sent SIGTERM");
}

void parent_catch_sigtstp(int signo) {
	PrintMessage("PARENT caught SIGTSTP");
	g_signal_caught = true;	
	if (g_bg_command_enable == false) {
		g_bg_command_enable = true;
		PrintMessage("Exiting fore-ground only mode");
	} else {
		g_bg_command_enable = false;
		PrintMessage("Entering fore-ground only mode");
	}
}

// Signal Handlers

void SetupSignalHandlers() {
	// SIGINT
	g_sigint_action.sa_flags = 0;
	g_sigint_action.sa_handler = parent_catch_sigint;
	sigfillset(&g_sigint_action.sa_mask);
	sigaction(SIGINT, &g_sigint_action, NULL);

	// SIGTERM
	g_sigterm_action.sa_flags = 0;
	g_sigterm_action.sa_handler = parent_catch_sigterm;
	sigfillset(&g_sigterm_action.sa_mask); 
	sigaction(SIGTERM, &g_sigterm_action, NULL);

	// SIGTSTP
	g_sigtstp_action.sa_flags = 0;
	g_sigtstp_action.sa_handler = parent_catch_sigtstp;
	sigfillset(&g_sigtstp_action.sa_mask); 
	sigaction(SIGTSTP, &g_sigtstp_action, NULL);
}

// For critical code segment where we don't want to be interrupted by blockable signals
void SetupBlockSignals() {
	sigemptyset(&g_blocked_signal_set);
	sigaddset(&g_blocked_signal_set, SIGINT);
	sigaddset(&g_blocked_signal_set, SIGTERM);
	sigaddset(&g_blocked_signal_set, SIGTSTP);
} 

// Debug purpose
void PrintTokens(FILE* out_file, char* tokens[], int num_tokens) {
	fprintf(out_file, "Tokens: ");
	int i;
	for (i = 0; i < num_tokens; ++i) {
		fprintf(out_file, "%s ", tokens[i]);	
	}
	printf("\n");
}

char* GetUserCommand(char* input_string) {
	input_string = NULL;
	size_t len = 0;
	// internally, getline here then use rellocate to alloc memory
	getline(&input_string, &len, stdin);
	// getline does read the '\n', so pick it off
	input_string[strlen(input_string)-1] = '\0';	
	return input_string;
}	

// look for old_substring in buffer and replace it with new_substring 
void ReplaceString(char* original_string, char* search_substring, char* replace_substring) {
	char buffer[MAX_ARG_LENGTH];
	char* found_pos = NULL;

	found_pos = strstr(original_string, search_substring);
	if (!found_pos) {
		return;
	}

	int prefix_length;
	prefix_length = found_pos - original_string;
	strncpy(buffer, original_string, prefix_length);
	buffer[prefix_length] = '\0';
	
	int suffix_length;
	suffix_length = strlen(search_substring);
	sprintf(buffer+prefix_length, "%s%s", replace_substring, found_pos+suffix_length);

	strcpy(original_string, buffer);
	
}

// replace substring "$$" by getid()
void ExpandDollaSign(char* command_tokens[], int num_tokens) {
	int i;
	for (i = 1; i < num_tokens; i++) {
		char pid[20];
		//char* buffer;
		memset(pid, '\0', sizeof(pid));
		sprintf(pid, "%d\0", (int)getpid());
		ReplaceString(command_tokens[i], "$$", pid);
	}
}

int ParseCommand(char input_string[], char* input_tokens[]) {
	const char delim[2] = " ";
	char input_string_cpy[50];
	int num_tokens = 0;

	// copy actual input string to a cpy version to use in strtok
	// because strtok modifies the source string
	memset(input_string_cpy, '\0', sizeof(input_string_cpy));
	strcpy(input_string_cpy, input_string);

	// Painful, annoying part - use strtok!
	char *token = NULL;
	token = strtok(input_string_cpy, delim); // result: token = command name
	int i = 0;
	while (token != NULL) {
		input_tokens[i] = (char*)calloc(MAX_TOKEN_LENGTH, sizeof(char));
		memset(input_tokens[i], '\0', MAX_TOKEN_LENGTH);
		strcpy(input_tokens[i], token);	
		i++;
		token = strtok(NULL, delim);
	}
	num_tokens = i;

	if (token != NULL) {
		//free(token);
		token = NULL;
	}
	return num_tokens;
}

void InitBackgroundPidArray() {
	int i;
	for (i = 0; i < MAX_BG_PID; i++) {
		g_background_pids[i] = DEFAULT_BG_PID;
	}
	g_num_background_pids = 0;
}

// Prepare for the first reading
void InitTokenBuffer(char* token_buffer[]) {
	int i;
	for (i = 0; i < MAX_NUM_TOKENS; ++i) {
		token_buffer[i] = NULL;
	}
}

// Clean up buffer for the next reading 
int ResetTokenBuffer(char* token_buffer[], int num_tokens) {
	int i;
	for (i = 0; i < num_tokens; ++i) {
		if (token_buffer[i] != NULL) {
			//free(token_buffer[i]);
			token_buffer[i] = NULL;
		}
	}
	num_tokens = 0;
	return num_tokens;	
}

void PrintChildExitStatus(pid_t actual_pid, int child_exit_status) {
	/*
 	* WIFEXITED return non-zero if the process terminates normally 
 	* WEXITSTATUS return actual exit status
 	*
 	* WIFSIGNALED return non-zero if the process was terminated by a signal
 	* WTERMISG return the terminating signal
 	* 
 	* Only ONE of the WIFEXITED() and WIFSIGNALED() will be non-zero
 	* => Use both to know how a child process died
 	*
 	* 
 	* */
	if (actual_pid == -1) {
		perror("Wait failed\n");
		exit(WAIT_FAILED_ERROR_CODE);	
	}
	if (WIFEXITED(child_exit_status) != 0) {
		g_last_exit_code = WEXITSTATUS(child_exit_status);
		g_was_terminated = false;
	}
	else if (WIFSIGNALED(child_exit_status != 0)) {
		printf("CHILD(%d) was terminated by signal ", actual_pid);
		printf("%d\n", WTERMSIG(child_exit_status));	
		g_last_terminate_signal_code = WTERMSIG(child_exit_status);
		g_was_terminated = true;
	}
}


// Block parent process until this child finish
void WaitChildBlock(pid_t child_pid) {
	int child_exit_status = DEFAULT_NEG_INT;
	pid_t actual_pid = DEFAULT_NEG_INT;

	//printf("Parent(%d) is waiting for child(%d) to terminate\n", getpid(), child_pid); // debug

	// block parent until the child process with 
	actual_pid = waitpid(child_pid, &child_exit_status, 0); 

	PrintChildExitStatus(actual_pid, child_exit_status);
}

// put a pid to the global array to interact later
void KeepTrackPid(pid_t child_pid) {
	g_background_pids[g_num_background_pids] = child_pid;
	g_num_background_pids++;
}


void PrintBackgroundPidBegins(pid_t child_pid) {
	fprintf(stdout, "Background process %d has begun\n", (int)child_pid);
}

// 
void CheckAndPrintCompletedProcs() {
	int child_exit_status = DEFAULT_NEG_INT;
	pid_t actual_pid = DEFAULT_NEG_INT;

	int i;
	for (i = 0; i < g_num_background_pids; i++) {
		// NOHANG: non-block waiting
		actual_pid = waitpid(g_background_pids[i], &child_exit_status, WNOHANG);
		if (actual_pid == g_background_pids[i]) {
			fprintf(stdout, "Background process %d has completed\n", (int)actual_pid);
			g_num_background_pids--;
			PrintChildExitStatus(actual_pid, child_exit_status);
		}
	}

	
}

void PrintReceivedCommand(int num_token, char* command_tokens[]) {
	int i;
	for (i = 0; i < num_token; ++i) {
		printf("%s ", command_tokens[i]);
	}
	printf("\n");
}

// delete character at the <pos>th  position
void ShiftLeftArrayFromPos(char* command_tokens[], int num_tokens, int pos) {
	int i;
	for (i = pos; i < num_tokens-1; i++) {
		memset(command_tokens[i], '\0', sizeof(command_tokens[i]));
		strcpy(command_tokens[i], command_tokens[i+1]);
	}
}
int RemoveElementByPosition(char* command_tokens[], int num_tokens, int pos) {
	ShiftLeftArrayFromPos(command_tokens, num_tokens, pos);
	if (command_tokens[num_tokens-1] != NULL) {
		//free(command_tokens[num_tokens-1]);
		command_tokens[num_tokens-1] = NULL;
	}
	num_tokens--;
	return num_tokens;
}

int RemoveRedirectionTokens(char* command_tokens[], int num_tokens, int pos) {
	num_tokens = RemoveElementByPosition(command_tokens, num_tokens, pos); // remove "<"
	num_tokens = RemoveElementByPosition(command_tokens, num_tokens, pos); // remove "path"
	return num_tokens;	
}

// find the "<" or the ">" symbol and return its position in tokens array
int FindRedirectionSymbol(char* command_tokens[], int num_tokens, const char* symbol) { 
	int i;
	int symbol_pos = NOT_FOUND;
	for (i = 0; i < num_tokens; i++) {
		if (!strcmp(command_tokens[i], symbol)) {
			symbol_pos = i;
			return symbol_pos;
		}
	}
	return symbol_pos;
}

// Pick off "<" and the pathname right next to it
// and set corresponding flags
int ProcessInputRedirection(char* command_tokens[], int num_tokens) {
	int i;
	int symbol_pos;

	symbol_pos = FindRedirectionSymbol(command_tokens, num_tokens, "<");
	if (symbol_pos == NOT_FOUND) { 
		g_file_src_path = NULL;
		return num_tokens;
	}

	g_file_src = open(command_tokens[symbol_pos+1], O_RDONLY);
	if (g_file_src == -1) {
		perror("error: source open()");
		exit(1);
	}

	g_file_src_path = (char*)calloc(MAX_TOKEN_LENGTH, sizeof(char));
	memset(g_file_src_path, '\0', sizeof(g_file_src_path));
	strcpy(g_file_src_path, command_tokens[symbol_pos+1]);
	
	num_tokens = RemoveRedirectionTokens(command_tokens, num_tokens, symbol_pos);
	return num_tokens;
}

// Pick off ">" and the pathname right next to it
// and set corresponding flags
int ProcessOutputRedirection(char* command_tokens[], int num_tokens) {
	int i;
	int symbol_pos;

	symbol_pos = FindRedirectionSymbol(command_tokens, num_tokens, ">");
	if (symbol_pos == NOT_FOUND) { 
		g_file_dest_path = NULL;
		return num_tokens;
	}

	g_file_dest = open(command_tokens[symbol_pos+1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (g_file_dest == -1) {
		perror("error: source open()");
		exit(1);
	}

	g_file_dest_path = (char*)calloc(MAX_TOKEN_LENGTH, sizeof(char));
	memset(g_file_dest_path, '\0', sizeof(g_file_dest_path));
	strcpy(g_file_dest_path, command_tokens[symbol_pos+1]);
	
	num_tokens = RemoveRedirectionTokens(command_tokens, num_tokens, symbol_pos);
	return num_tokens;
}

// NOTE: This also takes out "<" and ">" and pathname(s), if any, out of command_tokens
int ProcessIORedirection(char* command_tokens[], int num_tokens) {
	num_tokens = ProcessInputRedirection(command_tokens, num_tokens); 
	num_tokens = ProcessOutputRedirection(command_tokens, num_tokens); 
	return num_tokens;
}

void ResetInputRedirect(void) {
	g_file_src = 0;
	if (g_file_src_path != NULL) {
		//free(g_file_src_path);
		g_file_src_path = NULL;
	}
}

void ResetOutputRedirect(void) {
	g_file_dest = 1;
	if (g_file_dest_path != NULL) {
		//free(g_file_dest_path);
		g_file_dest_path = NULL;
	}
}

void ResetIORedirect(void) {
	ResetInputRedirect();
	ResetOutputRedirect();
}

void RedirectInput(void) {
	int redirect_result = -1; 
	redirect_result = dup2(g_file_src, 0);
	if (redirect_result == -1) {
		perror("source dup2()");
		exit(1);
	}
	
}

void RedirectOutput(void) {
	int redirect_result = -1;
	redirect_result = dup2(g_file_dest, 1);
	if (redirect_result == -1) {
		perror("dest dup2()");
		exit(1);
	}
}

// if IO is not standard IO, then redirect correspondingly
void RedirectIO(void) {
	if (g_file_src != 0) {
		RedirectInput();
	} 
	if (g_file_dest != 1) {
		RedirectOutput();
	}
}

// close upon exec if not standard IO
void CloseFilesOnExecute(void) {
	if (g_file_src != 0) { 
		fcntl(g_file_src, F_SETFD, FD_CLOEXEC);
	}
	if (g_file_dest != 1) {
		fcntl(g_file_dest, F_SETFD, FD_CLOEXEC);
	}
}

// set things up and fork a child to execute the non-built-in commands
// redirect if any
void ExecuteCommand(int num_tokens, char* command_tokens[]) {
	pid_t spawn_pid = DEFAULT_NEG_INT;

	sigprocmask(SIG_BLOCK, &g_blocked_signal_set, NULL);
	g_sigint_action.sa_handler = child_catch_sigint;
	g_sigterm_action.sa_handler = child_catch_sigterm;
	sigprocmask(SIG_UNBLOCK, &g_blocked_signal_set, NULL);

	spawn_pid = fork();

	if (spawn_pid == -1) {
		perror("Failed to fork new process");
		exit(FORK_FAILED_ERROR_CODE);
	}
	else if (spawn_pid == 0) { // sucessfully forked a new child process
		//
		// CHILD's code
		//
		ResetIORedirect();
		num_tokens = ProcessIORedirection(command_tokens, num_tokens);
		RedirectIO();		
		CloseFilesOnExecute();

		execvp(command_tokens[0], command_tokens);

		// if execute command failed
		perror("CHILD: exec failure!\n");
		exit(1);
	}
	//
	// PARENT's code
	//
	sigprocmask(SIG_BLOCK, &g_blocked_signal_set, NULL);
	g_sigint_action.sa_handler = parent_catch_sigint;
	g_sigterm_action.sa_handler = parent_catch_sigterm;
	sigprocmask(SIG_UNBLOCK, &g_blocked_signal_set, NULL);
		
	if (g_is_bg_command) {
		// non-block waiting
		PrintBackgroundPidBegins(spawn_pid);		
		KeepTrackPid(spawn_pid);
	} else {
		// block waiting
		sigprocmask(SIG_BLOCK, &g_blocked_signal_set, NULL);
		WaitChildBlock(spawn_pid);
		sigprocmask(SIG_UNBLOCK, &g_blocked_signal_set, NULL);
	}
	
	CheckAndPrintCompletedProcs();	
}

void PrintLastStatus() {
	if (g_was_terminated) {
		printf("terminated by signal %d\n", g_last_terminate_signal_code);
	}
	else {
		printf("exit value %d\n", g_last_exit_code);
	}
}

bool HasCommentSyntax(char token[]) {
	if (token[0] == '#') {
		return true;
	}
	return false;
}

bool IsEmptyCommand(int num_tokens) {
	if (num_tokens == 0) { // only has ':' in command
		return true;
	}
	return false;
}

void SetWorkingDirectory(char path[]) {
	memset(g_working_directory, '\0', sizeof(g_working_directory));
	strcpy(g_working_directory, path);
}

bool HasNoArgument(int num_tokens) {
	if (num_tokens == 2) {
		return true;	
	}
	return false;
}

bool IsValidCDCommand(char* command_tokens[], int num_tokens) {
	if (num_tokens != 1 && num_tokens != 2) {
		fprintf(stderr, "wrong number of arguments.\nCommand: ");
		PrintTokens(stderr, command_tokens, num_tokens);
		fprintf(stderr, "is not a valid CD command\n");
		return false;
	}
	return true;
}

// side effect: this will change current_working_dir
void GetCurrentWorkingDir(char current_working_dir[]) {
	char* getcwd_result;
	//getcwd_result = getcwd(current_working_dir, sizeof(current_working_dir));
	getcwd_result = getcwd(current_working_dir, PATH_MAX-1);
	if ( getcwd_result == NULL ) {
		perror("getcwd() error\n");	
		perror("Working Directory unchanged\n");
		return;
	} 
}

// using cwd()
void PrintCurrentWorkingDir() {
	char current_working_dir[PATH_MAX-1];
	memset(current_working_dir, '\0', PATH_MAX-1);
	GetCurrentWorkingDir(current_working_dir);
	fprintf(stdout, "Current working directory: %s\n", current_working_dir);
}

// using chdir()
void PrintChDirError() {
	perror("chdir() error\n");
	int error_code = errno; // errno is from <linux/limits.h>
	if (error_code == EACCES) perror("Search permission was denied\n");
	if (error_code == EFAULT) perror("new dir points outside your accessible address space\n");
	if (error_code == EIO) perror("An I/O error occurred\n");
	if (error_code == ELOOP) perror("Too many symbolic links were encountered in resolving new dir\n");
	if (error_code == ENOENT) perror("The new dir does not exist\n");
	if (error_code == ENOMEM) perror ("In sufficient kernel memory\n");
	if (error_code == ENOTDIR) perror("A component of new dir is not a directory\n");		
}

void ChangeDirToHome(char new_working_dir[]) {
	char* home_dir = NULL;
	home_dir = getenv("HOME");
	strcpy(new_working_dir, home_dir);
	int change_result;	
	change_result = chdir(new_working_dir);
	if (change_result != 0) {
		PrintChDirError();	
	}	
}


void SetCurrentWorkingDir(char* command_tokens[], int num_tokens) {
	char current_working_dir[PATH_MAX-1];
	memset(current_working_dir, '\0', PATH_MAX-1);
	char new_working_dir[PATH_MAX-1];
	memset(new_working_dir, '\0', PATH_MAX-1);
	char targeted_dir[PATH_MAX-1];
	memset(targeted_dir, '\0', PATH_MAX-1);

	// if no arg supplied, change dir to $HOME
	if (num_tokens == 1) {
		ChangeDirToHome(new_working_dir);
		return;
	}
	strcpy(targeted_dir, command_tokens[1]);

	// if it's absolute path, put it right on
	if (targeted_dir[0] == '/') {
		strcpy(new_working_dir, targeted_dir);
	// if it's relative path, concat it with current dir
	} else {
		GetCurrentWorkingDir(current_working_dir);	
		sprintf(new_working_dir, "%s/%s", current_working_dir, targeted_dir);
	}
	
	int change_dir_result;	
	change_dir_result = chdir(new_working_dir);
	if (change_dir_result != 0) {
		PrintChDirError();
	}
}

bool IsBackgroundCommand(char* command_tokens[], int num_token) {
	if (!strcmp(command_tokens[num_token-1], "&")) {
		return true;	
	}
	return false;
}

// NOTE: this will change command_tokens and num_tokens
int TrimAmpersand(char* command_tokens[], int num_tokens) {
	if (command_tokens[num_tokens-1] != NULL) {
		//free(command_tokens[num_tokens-1]);
		command_tokens[num_tokens-1] = NULL;
	}
	num_tokens--;
	return num_tokens;
}


int main(int in_argument_count, char ** in_arguments) {
	char* input_string = NULL; // input buffer
	int num_tokens = 0;
	char* command_tokens[MAX_NUM_TOKENS];

	// Signal config
	SetupBlockSignals();
	SetupSignalHandlers();

	InitTokenBuffer(command_tokens); // tokens arrays which args are parse into

	// Infinte user input loop
	while (1) {
		printf(": "); 

		g_signal_caught = false;

		input_string = GetUserCommand(input_string);	

		if (g_signal_caught) continue;

		
		num_tokens = ParseCommand(input_string, command_tokens);
		if (num_tokens == -1) {
			ResetTokenBuffer(command_tokens, num_tokens); 
			perror("Bad file");
			continue;
		}

		if (IsEmptyCommand(num_tokens) || HasCommentSyntax(command_tokens[0]) ) {
			ResetTokenBuffer(command_tokens, num_tokens);
			continue;
		}

		// If background command, set flags
		if (IsBackgroundCommand(command_tokens, num_tokens)) {
			num_tokens = TrimAmpersand(command_tokens, num_tokens);	
			if (g_bg_command_enable == true) {
				g_is_bg_command = true;
			} else {
				g_is_bg_command = false;
			}
		} else {
			g_is_bg_command = false;
		}
		
		ExpandDollaSign(command_tokens, num_tokens);
		
		// Get command name	
		char command_name[MAX_ARG_LENGTH];
		memset(command_name, '\0', sizeof(command_name));	
		strcpy(command_name, command_tokens[0]);

		// Built-in commands
		if (strcmp(command_name, "exit") == 0) {
			exit(0);	
		} else if (strcmp(command_name, "cd") == 0) {
			PrintCurrentWorkingDir();
			if(!IsValidCDCommand(command_tokens, num_tokens)) continue;
			SetCurrentWorkingDir(command_tokens, num_tokens);
			PrintCurrentWorkingDir();
		} else if (strcmp(command_name, "status") == 0) {
			PrintLastStatus();			
		} else {
			// Fork a child and have that child execute command
			ExecuteCommand(num_tokens, command_tokens);
		}
		// Clean command token array every loop before read new input
		ResetTokenBuffer(command_tokens, num_tokens); 
	}
	
	return 0;
}
	
	
	
	

