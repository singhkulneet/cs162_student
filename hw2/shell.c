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

/* Counts how many separate pipeline commands exist in the tokens */
int count_pipeline_stages(struct tokens *tokens) {
  size_t num_tokens = tokens_get_length(tokens);
  int stages = 1;
  for (size_t i = 0; i < num_tokens; i++) {
    if (strcmp(tokens_get_token(tokens, i), "|") == 0) {
      stages++;
    }
  }
  return stages;
}

typedef struct cmd_stage_t {
  char **args;       // NULL-terminated array of arguments for execv
  char *input_file;  // NULL if no '<' redirection
  char *output_file; // NULL if no '>' redirection
} cmd_stage_t;

cmd_stage_t parse_stage(struct tokens *tokens, size_t *token_idx) {
  cmd_stage_t stage = { .args = NULL, .input_file = NULL, .output_file = NULL };
  size_t num_tokens = tokens_get_length(tokens);

  stage.args = malloc((num_tokens + 1) * sizeof(char *));
  size_t cmd_argc = 0;

  while (*token_idx < num_tokens) {
    char *token = tokens_get_token(tokens, *token_idx);
    (*token_idx)++;

    if (strcmp(token, "|") == 0) {
      break;
    } else if (strcmp(token, "<") == 0) {
      stage.input_file = tokens_get_token(tokens, (*token_idx)++);
    } else if (strcmp(token, ">") == 0) {
      stage.output_file = tokens_get_token(tokens, (*token_idx)++);
    } else {
      stage.args[cmd_argc++] = token;
    }
  }
  stage.args[cmd_argc] = NULL;
  return stage;
}

typedef struct {
    int read;   // Corresponds to fd[0]
    int write;  // Corresponds to fd[1]
} pipe_t;

void execute_child_stage(cmd_stage_t *stage, int stage_idx, int num_stages, pipe_t *pipe_fds) {
  // Handle Pipe Input (from previous stage)
  if (stage_idx > 0) {
    dup2(pipe_fds[stage_idx - 1].read, STDIN_FILENO);
  }
  // Explicit file override (<)
  if (stage->input_file) {
    int in_fd = open(stage->input_file, O_RDONLY);
    if (in_fd < 0) { perror("open input failed"); exit(EXIT_FAILURE); }
    dup2(in_fd, STDIN_FILENO);
    close(in_fd);
  }

  // Handle Pipe Output (to next stage)
  if (stage_idx < num_stages - 1) {
    dup2(pipe_fds[stage_idx].write, STDOUT_FILENO);
  }
  // Explicit file override (>)
  if (stage->output_file) {
    int out_fd = open(stage->output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) { perror("open output failed"); exit(EXIT_FAILURE); }
    dup2(out_fd, STDOUT_FILENO);
    close(out_fd);
  }

  // Close ALL pipe ends in the child context
  for (int j = 0; j < num_stages - 1; j++) {
    close(pipe_fds[j].read);
    close(pipe_fds[j].write);
  }

  // Resolve path and run
  char *full_path = resolve_path(stage->args[0]);
  if (!full_path) {
    fprintf(stderr, "%s: command not found\n", stage->args[0]);
    free(stage->args);
    exit(EXIT_FAILURE);
  }

  execv(full_path, stage->args);
  perror("execv failed");
  free(stage->args);
  free(full_path);
  exit(EXIT_FAILURE);
}

void execute_pipeline(struct tokens *tokens) {
  int num_stages = count_pipeline_stages(tokens);
  pid_t pids[num_stages];
  pipe_t pipe_fds[num_stages - 1];

  // Initialize all pipes
  for (int i = 0; i < num_stages - 1; i++) {
    if (pipe((int *)&pipe_fds[i]) < 0) {
      perror("pipe failed");
      return;
    }
  }

  size_t token_idx = 0;
  for (int i = 0; i < num_stages; i++) {
    cmd_stage_t stage = parse_stage(tokens, &token_idx);

    if (stage.args[0] == NULL) {
      free(stage.args);
      continue;
    }

    pids[i] = fork();
    if (pids[i] < 0) {
      perror("fork failed");
      exit(EXIT_FAILURE);
    }
    else if (pids[i] == 0) {
      execute_child_stage(&stage, i, num_stages, pipe_fds);
    }

    free(stage.args); // Clean up the parsed args structure in parent
  }

  // Close all pipes in parent so EOF signals propagate
  for (int i = 0; i < num_stages - 1; i++) {
    close(pipe_fds[i].read);
    close(pipe_fds[i].write);
  }

  // Wait for all processes to terminate
  for (int i = 0; i < num_stages; i++) {
    int status;
    waitpid(pids[i], &status, 0);
  }
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
      execute_pipeline(tokens);
    }

    if (shell_is_interactive)
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);

    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}
