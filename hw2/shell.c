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

/* --- JOB CONTROL STRUCTURES --- */
typedef struct job {
  pid_t pgid;                 /* Process Group ID */
  char *cmd_string;           /* Saved command text representation */
  struct termios tmodes;      /* Saved terminal settings for this specific job */
  bool is_stopped;            /* Is the job currently suspended via Ctrl-Z? */
  struct job *next;
} job_t;

job_t *job_list = NULL;       /* Head of global active tracking jobs list */

/* Helper to add a new tracked job */
void add_job(pid_t pgid, const char *cmd, struct termios modes, bool stopped) {
  job_t *new_job = malloc(sizeof(job_t));
  new_job->pgid = pgid;
  new_job->cmd_string = strdup(cmd);
  new_job->tmodes = modes;
  new_job->is_stopped = stopped;
  new_job->next = job_list;
  job_list = new_job;
}

/* Helper to find a tracked job by pgid or primary pid */
job_t *find_job(pid_t pgid) {
  job_t *curr = job_list;
  while (curr) {
    if (curr->pgid == pgid) return curr;
    curr = curr->next;
  }
  return NULL;
}

/* Helper to remove a job upon termination cleanup */
void remove_job(pid_t pgid) {
  job_t *curr = job_list;
  job_t *prev = NULL;
  while (curr) {
    if (curr->pgid == pgid) {
      if (prev) prev->next = curr->next;
      else job_list = curr->next;
      free(curr->cmd_string);
      free(curr);
      return;
    }
    prev = curr;
    curr = curr->next;
  }
}

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
int cmd_wait(struct tokens *tokens);
int cmd_fg(struct tokens *tokens);
int cmd_bg(struct tokens *tokens);
int cmd_jobs(struct tokens *tokens);

fun_desc_t cmd_table[] = {
  {cmd_help, "?", "show this help menu"},
  {cmd_cd, "cd", "change current working directory"},
  {cmd_pwd, "pwd", "get current working directory"},
  {cmd_exit, "exit", "exit the command shell"},
  {cmd_wait, "wait", "wait until all background jobs have terminated"},
  {cmd_fg, "fg", "move a process to the foreground"},
  {cmd_bg, "bg", "resume a paused background process"},
  {cmd_jobs, "jobs", "list all active and suspended background tasks"},
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

/* Built-in: wait */
int cmd_wait(unused struct tokens *tokens) {
  int status;
  pid_t pid;
  /* Block and reap all children in the system */
  while ((pid = waitpid(-1, &status, WUNTRACED)) > 0) {
    if (WIFSTOPPED(status)) {
      job_t *j = find_job(pid);
      if (j) j->is_stopped = true;
    } else {
      remove_job(pid);
    }
  }
  return 1;
}

/* Helper to wait on a foreground process group safely */
void wait_for_job(job_t *j) {
  int status;
  pid_t pid;
  
  /* Wait specifically for this process group's leader or members */
  while ((pid = waitpid(-j->pgid, &status, WUNTRACED)) > 0) {
    if (WIFSTOPPED(status)) {
      j->is_stopped = true;
      printf("\n[%d] Stopped\n", j->pgid);
      break;
    } else if (WIFSIGNALED(status) || WIFEXITED(status)) {
      /* If the leader or tracking processes exit, strip out the job */
      remove_job(j->pgid);
      break;
    }
  }
}

/* Built-in: fg [pid] */
int cmd_fg(struct tokens *tokens) {
  job_t *target = NULL;
  char *arg = tokens_get_token(tokens, 1);

  if (arg) {
    pid_t requested_pid = atoi(arg);
    target = find_job(requested_pid);
  } else {
    /* Grab most recently launched job if no pid provided */
    target = job_list; 
  }

  if (!target) {
    fprintf(stderr, "fg: no such job found\n");
    return 1;
  }

  if (shell_is_interactive) {
    /* Hand over terminal control to the job's process group */
    tcsetpgrp(shell_terminal, target->pgid);
    tcsetattr(shell_terminal, TCSADRAIN, &target->tmodes);
  }

  /* Resume the process group if it was stopped */
  kill(-target->pgid, SIGCONT);
  target->is_stopped = false;

  wait_for_job(target);

  /* Reclaim terminal control upon completion or re-suspension */
  if (shell_is_interactive) {
    tcsetpgrp(shell_terminal, shell_pgid);
    tcsetattr(shell_terminal, TCSADRAIN, &shell_tmodes);
  }
  return 1;
}

/* Built-in: bg [pid] */
int cmd_bg(struct tokens *tokens) {
  job_t *target = NULL;
  char *arg = tokens_get_token(tokens, 1);

  if (arg) {
    pid_t requested_pid = atoi(arg);
    target = find_job(requested_pid);
  } else {
    target = job_list; 
  }

  if (!target) {
    fprintf(stderr, "bg: no such job found\n");
    return 1;
  }

  /* Resume the background job without transferring terminal control */
  fprintf(stderr, "Restarting: %s\n", target->cmd_string);
  kill(-target->pgid, SIGCONT);
  target->is_stopped = false;
  return 1;
}

/* Built-in: jobs */
int cmd_jobs(unused struct tokens *tokens) {
  job_t *curr = job_list;
  
  if (!curr) {
    printf("No active background or stopped jobs.\n");
    return 1;
  }

  printf("  PGID\tState\t\tCommand\n");
  printf("----------------------------------------\n");

  while (curr) {
    int status;
    // WNOHANG makes waitpid check the state and return immediately without blocking
    pid_t result = waitpid(-curr->pgid, &status, WNOHANG | WUNTRACED);

    if (result > 0) {
      if (WIFSTOPPED(status)) {
        curr->is_stopped = true;
      } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
        // The process finished or was terminated; we need to drop it.
        job_t *to_delete = curr;
        curr = curr->next; // Advance iterator before deletion
        remove_job(to_delete->pgid);
        continue;
      }
    }

    /* Clean up the trailing newline from fgets when printing the command string */
    char clean_cmd[256];
    strncpy(clean_cmd, curr->cmd_string, sizeof(clean_cmd) - 1);
    clean_cmd[strcspn(clean_cmd, "\n")] = '\0';

    /* Display the job state profile */
    printf("[%d]\t%s\t%s\n", 
           curr->pgid, 
           curr->is_stopped ? "Stopped" : "Running", 
           clean_cmd);

    curr = curr->next;
  }
  return 1;
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
  shell_terminal = STDIN_FILENO;
  shell_is_interactive = isatty(shell_terminal);

  if (shell_is_interactive) {
    while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
      kill(-shell_pgid, SIGTTIN);

    shell_pgid = getpid();
    tcsetpgrp(shell_terminal, shell_pgid);
    tcgetattr(shell_terminal, &shell_tmodes);

    struct sigaction sa;
    sa.sa_handler = SIG_IGN; 
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, NULL);  
    sigaction(SIGTSTP, &sa, NULL); 
    sigaction(SIGTTIN, &sa, NULL); 
    sigaction(SIGTTOU, &sa, NULL); 
  }
}

char *resolve_path(char *cmd) {
  if (!cmd) return NULL;
  if (strchr(cmd, '/') != NULL) {
    if (access(cmd, X_OK) == 0) return strdup(cmd);
    return NULL;
  }
  char *path_env = getenv("PATH");
  if (!path_env) return NULL;

  char *path_cpy = strdup(path_env);
  char *saveptr;
  char *dir = strtok_r(path_cpy, ":", &saveptr);
  char *resolved = NULL;

  while (dir != NULL) {
    char potential_path[PATH_MAX];
    snprintf(potential_path, sizeof(potential_path), "%s/%s", dir, cmd);
    if (access(potential_path, X_OK) == 0) {
      resolved = strdup(potential_path);
      break; 
    }
    dir = strtok_r(NULL, ":", &saveptr);
  }
  free(path_cpy); 
  return resolved;
}

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
  char **args;       
  char *input_file;  
  char *output_file; 
} cmd_stage_t;

cmd_stage_t parse_stage(struct tokens *tokens, size_t *token_idx) {
  cmd_stage_t stage = { .args = NULL, .input_file = NULL, .output_file = NULL };
  size_t num_tokens = tokens_get_length(tokens);

  stage.args = malloc((num_tokens + 1) * sizeof(char *));
  size_t cmd_argc = 0;

  while (*token_idx < num_tokens) {
    char *token = tokens_get_token(tokens, *token_idx);
    
    /* Break early if we hit a pipeline descriptor or background modifier */
    if (strcmp(token, "|") == 0 || strcmp(token, "&") == 0) {
      break;
    }

    (*token_idx)++;

    if (strcmp(token, "<") == 0) {
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
    int read;   
    int write;  
} pipe_t;

void execute_child_stage(cmd_stage_t *stage, int stage_idx, int num_stages, pipe_t *pipe_fds, pid_t pgid, bool is_background) {
  if (shell_is_interactive) {
    pid_t current_pgid = (stage_idx == 0) ? 0 : pgid;
    setpgid(0, current_pgid);

    /* ONLY transfer terminal control if it's a foreground execution layout */
    if (stage_idx == 0 && !is_background) {
      tcsetpgrp(shell_terminal, getpid());
    }

    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTSTP, &sa, NULL);
    sigaction(SIGTTIN, &sa, NULL);
    sigaction(SIGTTOU, &sa, NULL);
  }

  if (stage_idx > 0) dup2(pipe_fds[stage_idx - 1].read, STDIN_FILENO);
  if (stage->input_file) {
    int in_fd = open(stage->input_file, O_RDONLY);
    if (in_fd < 0) { perror("open input failed"); exit(EXIT_FAILURE); }
    dup2(in_fd, STDIN_FILENO);
    close(in_fd);
  }

  if (stage_idx < num_stages - 1) dup2(pipe_fds[stage_idx].write, STDOUT_FILENO);
  if (stage->output_file) {
    int out_fd = open(stage->output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) { perror("open output failed"); exit(EXIT_FAILURE); }
    dup2(out_fd, STDOUT_FILENO);
    close(out_fd);
  }

  for (int j = 0; j < num_stages - 1; j++) {
    close(pipe_fds[j].read); close(pipe_fds[j].write);
  }

  char *full_path = resolve_path(stage->args[0]);
  if (!full_path) {
    fprintf(stderr, "%s: command not found\n", stage->args[0]);
    free(stage->args);
    exit(EXIT_FAILURE);
  }

  execv(full_path, stage->args);
  exit(EXIT_FAILURE);
}

void execute_pipeline(struct tokens *tokens, const char *raw_line) {
  int num_stages = count_pipeline_stages(tokens);
  pid_t pids[num_stages];
  pipe_t pipe_fds[num_stages - 1];

  /* Determine if this job runs in the background */
  bool is_background = false;
  size_t total_tokens = tokens_get_length(tokens);
  if (total_tokens > 0) {
    char *last_token = tokens_get_token(tokens, total_tokens - 1);
    if (strcmp(last_token, "&") == 0) {
      is_background = true;
    }
  }

  for (int i = 0; i < num_stages - 1; i++) {
    if (pipe((int *)&pipe_fds[i]) < 0) { perror("pipe failed"); return; }
  }

  pid_t pgid = 0;
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
      execute_child_stage(&stage, i, num_stages, pipe_fds, pgid, is_background);
    }
    else {
      if (shell_is_interactive) {
        if (i == 0) pgid = pids[0];
        setpgid(pids[i], pgid);
      }
    }
    free(stage.args); 
  }

  for (int i = 0; i < num_stages - 1; i++) {
    close(pipe_fds[i].read); close(pipe_fds[i].write);
  }

  /* Track the job globally */
  if (shell_is_interactive && pgid > 0) {
    add_job(pgid, raw_line, shell_tmodes, false);
  }

  /* Foreground vs Background waiting paths */
  if (!is_background) {
    job_t *j = find_job(pgid);
    if (j) {
      wait_for_job(j);
    } else {
      for (int i = 0; i < num_stages; i++) {
        int status;
        waitpid(pids[i], &status, WUNTRACED);
      }
    }

    if (shell_is_interactive) {
      tcsetpgrp(shell_terminal, shell_pgid);
      tcsetattr(shell_terminal, TCSADRAIN, &shell_tmodes);
    }
  } else {
    /* Background Job started: print tracing info without blocking */
    printf("[%d] Launched in background\n", pgid);
  }
}

int main(unused int argc, unused char *argv[]) {
  init_shell();

  static char line[4096];
  int line_num = 0;

  if (shell_is_interactive)
    fprintf(stdout, "%d: ", line_num);

  while (fgets(line, 4096, stdin)) {
    struct tokens *tokens = tokenize(line);
    if (tokens_get_length(tokens) == 0) {
      tokens_destroy(tokens);
      goto prompt;
    }

    int fundex = lookup(tokens_get_token(tokens, 0));

    if (fundex >= 0) {
      cmd_table[fundex].fun(tokens);
    } else {
      /* Pass the raw line buffer to help initialize descriptive job string trackers */
      execute_pipeline(tokens, line);
    }

    tokens_destroy(tokens);

prompt:
    if (shell_is_interactive)
      fprintf(stdout, "%d: ", ++line_num);
  }

  return 0;
}