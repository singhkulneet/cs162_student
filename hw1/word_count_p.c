/*
 * Implementation of the word_count interface using Pintos lists and pthreads.
 *
 * You may modify this file, and are expected to modify it.
 */

/*
 * Copyright © 2019 University of California, Berkeley
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef PINTOS_LIST
#error "PINTOS_LIST must be #define'd when compiling word_count_lp.c"
#endif

#ifndef PTHREADS
#error "PTHREADS must be #define'd when compiling word_count_lp.c"
#endif

#include "word_count.h"

void init_words(word_count_list_t *wclist) {
  list_init(&wclist->lst);
  pthread_mutex_init(&wclist->lock, NULL);
}

size_t len_words(word_count_list_t *wclist) {
  size_t num_words = 0;
  struct list_elem *e;

  pthread_mutex_lock(&wclist->lock);
  for (e = list_begin (&wclist->lst); e != list_end (&wclist->lst); e = list_next (e)) num_words++;
  pthread_mutex_unlock(&wclist->lock);

  return num_words;
}

word_count_t *find_word(word_count_list_t *wclist, char *word) {
  word_count_t *found_node = NULL;
  struct list_elem *e;

  pthread_mutex_lock(&wclist->lock);
  for (e = list_begin (&wclist->lst); e != list_end (&wclist->lst); e = list_next (e)) {
    word_count_t *node = list_entry (e, word_count_t, elem);
    if (strcmp(word, node->word) == 0) {
      found_node = node;
      break;
    }
  }
  pthread_mutex_unlock(&wclist->lock);

  return found_node;
}

word_count_t *add_word(word_count_list_t *wclist, char *word) {
  if (!wclist || !word) return NULL;

  word_count_t *node = NULL;
  struct list_elem *e;
  pthread_mutex_lock(&wclist->lock);
  for (e = list_begin (&wclist->lst); e != list_end (&wclist->lst); e = list_next (e)) {
    word_count_t *tmp = list_entry (e, word_count_t, elem);
    if (strcmp(word, tmp->word) == 0) {
      node = tmp;
      break;
    }
  }

  if (node) {
    node->count++;
    pthread_mutex_unlock(&wclist->lock);
  } else {
    node = malloc(sizeof(word_count_t));
    if (!node) {
      pthread_mutex_unlock(&wclist->lock);
      return NULL;
    }
    node->word = strdup(word);
    if (!node->word) {
      pthread_mutex_unlock(&wclist->lock);
      free(node);
      return NULL;
    }
    node->count = 1;
    list_push_front(&wclist->lst, &node->elem);
    pthread_mutex_unlock(&wclist->lock);
  }

  return node;
}

void fprint_words(word_count_list_t *wclist, FILE *outfile) { // does not need to be threadsafe
  struct list_elem *e;
  for (e = list_begin (&wclist->lst); e != list_end (&wclist->lst);
        e = list_next (e))
  {
    word_count_t *node = list_entry (e, word_count_t, elem);
    fprintf(outfile, "%*i\t%s\n", 8, node->count, node->word);
  }
}

static bool less_list(const struct list_elem *ewc1,
                      const struct list_elem *ewc2, void *aux) {
  word_count_t *node1 = list_entry (ewc1, word_count_t, elem);
  word_count_t *node2 = list_entry (ewc2, word_count_t, elem);
  return ((bool (*)(const word_count_t *, const word_count_t *))aux)(node1, node2);
}

void wordcount_sort(word_count_list_t *wclist,
                    bool less(const word_count_t *, const word_count_t *)) {
  list_sort(&wclist->lst, less_list, less);
}
