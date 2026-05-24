#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <limits.h>

#include "tokenizer.h"

/* Convenience macro to silence compiler warnings about unused function parameters. */
#define unused __attribute__((unused))

/* Whether the shell is connected to an actual terminal or not. */
bool shell_is_interactive;

/* File descriptor for the shell input */
int shell_terminal;

/* Terminal mode settings for the shell */
struct termios shell_tmodes;

/* Process group id for the shell */
pid_t shell_pgid;

/* Built-in command functions take token array (see parse.h) and return int */
typedef int cmd_fun_t(struct tokens *tokens);

/* Built-in command struct and lookup table */
typedef struct fun_desc {
  cmd_fun_t *fun;
  char *cmd;
  char *doc;
} fun_desc_t;

/* Function Prototypes */
int cmd_exit(struct tokens *tokens);
int cmd_pwd(struct tokens *tokens);
int cmd_cd(struct tokens *tokens);
int cmd_help(struct tokens *tokens);

fun_desc_t cmd_table[] = {
  {cmd_help, "?", "show this help menu"},
  {cmd_cd, "cd", "change current working directory"},
  {cmd_pwd, "pwd", "get current working directory"},
  {cmd_exit, "exit", "exit the command shell"},
};

/* Prints a helpful description for the given command */
int cmd_help(unused struct tokens *tokens) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    printf("%s - %s\n", cmd_table[i].cmd, cmd_table[i].doc);
  return 1;
}

int cmd_pwd(unused struct tokens *tokens) {
  char path[PATH_MAX];
  getcwd(path, PATH_MAX);
  printf("%s\n", path);
  return 1;
}

int cmd_cd(struct tokens *tokens) {
  if(chdir(tokens_get_token(tokens, 1)) != 0){
    fprintf(stderr, "Error failed to cd: %s\n", strerror(errno));
  }
  return 1;
}

/* Exits this shell */
int cmd_exit(unused struct tokens *tokens) {
  exit(0);
}

/* Looks up the built-in command, if it exists. */
int lookup(char cmd[]) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0))
      return i;
  return -1;
}

/* Intialization procedures for this shell */
void init_shell() {
  /* Our shell is connected to standard input. */
  shell_terminal = STDIN_FILENO;

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

    /* Take control of the terminal */
    tcsetpgrp(shell_terminal, shell_pgid);

    /* Save the current termios to a variable, so it can be restored later. */
    tcgetattr(shell_terminal, &shell_tmodes);
  }
}

char *resolve_path(char *cmd) {
  if (!cmd) return NULL;

  // Check if cmd already has a slash and is executable
  if (strchr(cmd, '/') != NULL) {
    if (access(cmd, X_OK) == 0) {
      return strdup(cmd);
    }
    return NULL;
  }

  // Retrieve the PATH environment variable
  char *path_env = getenv("PATH");
  if (!path_env) return NULL;

  // Duplicate the PATH string because strtok_r modifies the string it parses
  char *path_cpy = strdup(path_env);
  char *saveptr;
  char *dir = strtok_r(path_cpy, ":", &saveptr);
  char *resolved = NULL;

  // Loop through every directory listed in PATH
  while (dir != NULL) {
    char potential_path[PATH_MAX];
    
    // Construct the full path: "directory/command"
    snprintf(potential_path, sizeof(potential_path), "%s/%s", dir, cmd);

    // Check if this constructed path exists and is executable
    if (access(potential_path, X_OK) == 0) {
      resolved = strdup(potential_path);
      break; // Found it! Stop searching
    }

    dir = strtok_r(NULL, ":", &saveptr);
  }

  free(path_cpy); // Clean up our duplicate copies
  return resolved;
}

int main(unused int argc, unused char *argv[]) {
  init_shell();

  static char line[4096];
  int line_num = 0;

  /* Please only print shell prompts when standard input is not a tty */
  if (shell_is_interactive)
    fprintf(stdout, "%d: ", line_num);

  while (fgets(line, 4096, stdin)) {
    /* Split our line into words. */
    struct tokens *tokens = tokenize(line);

    /* Find which built-in function to run. */
    int fundex = lookup(tokens_get_token(tokens, 0));

    if (fundex >= 0) {
      cmd_table[fundex].fun(tokens);
    } else {
      size_t num_tokens = tokens_get_length(tokens);
      if (num_tokens > 0) {
        // Try to resolve the program path
        char *cmd_name = tokens_get_token(tokens, 0);
        char *full_path = resolve_path(cmd_name);

        if (full_path == NULL) {
          fprintf(stderr, "%s: command not found\n", cmd_name);
          if (shell_is_interactive) fprintf(stdout, "%d: ", ++line_num);
          continue; // Skip forking entirely and prompt again
        }
        
        char **args = malloc((num_tokens + 1) * sizeof(char *)); // allocated argv vector
        if (!args) {
          perror("malloc failed");
          free(full_path);
          exit(EXIT_FAILURE);
        }

        for (size_t i=0; i<num_tokens; i++) {
          args[i] = tokens_get_token(tokens, i);
        }
        args[num_tokens] = NULL; // Null terminating array

        // Fork the process
        pid_t pid = fork();

        if (pid < 0) {
          // forking failed
          perror("fork failed");
        } else if (pid == 0) { // child process
          execv(full_path, args); // full path of program

          // if execv returns, it means there was an error
          fprintf(stderr, "%s: failed to execute program\n", args[0]);
          free(args);
          free(full_path);
          exit(EXIT_FAILURE); // Stop new process from shell loop
        } else { // parent process
          int status;
          // Parent block and wait for child to complete
          waitpid(pid, &status, 0);
        }
        free(args); // Clean up pointer array
        free(full_path);
      }
    }

    if (shell_is_interactive)
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);

    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}
