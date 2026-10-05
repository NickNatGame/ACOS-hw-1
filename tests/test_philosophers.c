#include "soft_assert.h"

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>

static int fl_err = 0;
static char binary[PATH_MAX]; /* Тут сохраняем путь к исполняемому файлу*/

/* Размер файла узнаём заранее, чтобы не ограничивать длину вывода */
static char *read_file(const char *name) {
  FILE *file = fopen(name, "r");
  if (file == NULL) {
    return NULL;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }

  long size = ftell(file); /* смотрим позицию в файле */
  if (size < 0) {
    fclose(file);
    return NULL;
  }

  rewind(file); /* возвращаем позицию в начало */
  char *text = calloc((size_t)size + 1, 1); /* последний бит добавляем \0 */
  if (text != NULL) {
    SOFT_ASSERT_ERR(fread(text, 1, (size_t)size, file) == (size_t)size);
  }

  SOFT_ASSERT_ERR(fclose(file) == 0);
  return text;
}

/* Запускаем программу отдельно и сохраняем её вывод в файл
              По мотивам последнего семинара <3                       */
static char *run_program_with_signal(char *const args[], int expected_code,
                                     int has_log, int signal_number) {
  char directory[] = "/tmp/philosophers-test-XXXXXX"; /* тут мы создаем файл для теста, где XXXXXX мы заменяем уникальным окончанием  */
  char *created = mkdtemp(directory);                 /* а вот тут как раз заменяем                                                   */

  SOFT_ASSERT_ERR(created != NULL);
  if (created == NULL) {
    return NULL;
  }

  /* создаем ребенка для еще одного процесса */
  pid_t child = fork();
  SOFT_ASSERT_ERR(child >= 0);

  if (child == 0) {
    alarm(5); /* Защита от зависания программы */
    if (chdir(directory) != 0) {
      _exit(125);
    }

    int file = open("output.txt", O_WRONLY | O_CREAT | O_TRUNC, 0600);

    /* Если файл не открылся или не удалось перенаправить stdout или не удалось перенаправить
    stderr -> ошибко и ребенок заывершается*/
    if (file < 0 || dup2(file, STDOUT_FILENO) < 0 ||
        dup2(file, STDERR_FILENO) < 0) {
      _exit(125);
    }

    close(file);
    execv(binary, args);
    _exit(125);
  }

  /* тут родительский процесс */
  if (child > 0) {
    if (signal_number != 0) {
      char log_path[PATH_MAX];
      snprintf(log_path, sizeof(log_path), "%s/philosophers.log", directory);
      int ready = 0;
      struct timespec pause = {0, 10000000L};
      /* Ждём начала еды, чтобы не послать сигнал до настройки обработчика. */
      for (int i = 0; i < 300 && !ready; i++) {
        char *log = read_file(log_path);
        ready = log != NULL && strstr(log, "начал есть") != NULL;
        free(log);
        if (!ready) {
          nanosleep(&pause, NULL);
        }
      }
      SOFT_ASSERT_ERR(ready);
      if (ready) {
        SOFT_ASSERT_ERR(kill(child, signal_number) == 0);
      }
    }
    int status = 0;
    pid_t result;

    /* ждем окончания работы конкретного ребенка */
    do {
      result = waitpid(child, &status, 0);
    } while (result < 0 && errno == EINTR);

    SOFT_ASSERT_ERR(result == child);
    if (result == child) {
      /* Проверяет завершился ли процесс обычным выходом */
      SOFT_ASSERT_ERR(WIFEXITED(status));
      if (WIFEXITED(status)) {
        SOFT_ASSERT_ERR(WEXITSTATUS(status) == expected_code);
      }

    }
  }

  /* создал буферы и сделал пути для вывода и лога */
  char output_path[PATH_MAX], log_path[PATH_MAX];
  snprintf(output_path, sizeof(output_path), "%s/output.txt", directory);
  snprintf(log_path, sizeof(log_path), "%s/philosophers.log", directory);
  char *output = read_file(output_path);
  SOFT_ASSERT_ERR(output != NULL);

  SOFT_ASSERT_ERR((access(log_path, F_OK) == 0) == has_log);
  if (has_log) {
    char *log = read_file(log_path);
    SOFT_ASSERT_ERR(log != NULL);

    if (output != NULL && log != NULL) {
      SOFT_ASSERT_ERR(strcmp(output, log) == 0);
    }
    free(log);
  }

  /* удаляем временные файлы */
  unlink(output_path);
  unlink(log_path);
  SOFT_ASSERT_ERR(rmdir(directory) == 0);
  return output;
}

/* Обычные тесты запускаются без отправки сигнала. */
static char *run_program(char *const args[], int expected_code, int has_log) {
  return run_program_with_signal(args, expected_code, has_log, 0);
}

/* проверяем, что вывод существует и содержит нужный фрагмент */
static void check_text(const char *output, const char *text) {
  SOFT_ASSERT_ERR(output != NULL && strstr(output, text) != NULL);
}

/* по журналу проверяем, кому выдавались вилки и кто их освободил */
static void check_forks(const char *output, int count) {
  if (output == NULL) {
    return;
  }

  int *owners = calloc((size_t)count, sizeof(int));
  char *copy = strdup(output);
  SOFT_ASSERT_ERR(owners != NULL && copy != NULL);

  if (owners == NULL || copy == NULL) {
    free(owners);
    free(copy);
    return;
  }

  char *line = strtok(copy, "\n");
  while (line != NULL) {
    long long time;
    int id, offset = 0;

    /* разбираем строку */
    sscanf(line, "[%lld ms] Философ %d: %n", &time, &id, &offset);
    if (offset > 0) {
      SOFT_ASSERT_ERR(id >= 1 && id <= count);

      /* проверяем допустимый ли id */
      if (id < 1 || id > count) {
        line = strtok(NULL, "\n");
        continue;
      }

      int left = id - 1, right = id % count;
      int fork_number, first, second;
      const char *event = line + offset;

      /* проверка на соответствие вилок*/
      if (sscanf(event, "получил вилку %d", &fork_number) == 1 ||
          sscanf(event, "получил левую вилку %d", &fork_number) == 1 ||
          sscanf(event, "получил правую вилку %d", &fork_number) == 1) {

        SOFT_ASSERT_ERR(fork_number == left + 1 || fork_number == right + 1);

        if (fork_number == left + 1 || fork_number == right + 1) {
          SOFT_ASSERT_ERR(owners[fork_number - 1] == 0);
          owners[fork_number - 1] = id;
        }
      } else if (strncmp(event, "начал есть", strlen("начал есть")) == 0) {
        SOFT_ASSERT_ERR(owners[left] == id && owners[right] == id);
      } else if (sscanf(event, "освободил вилки %d и %d", &first, &second) == 2) {
        SOFT_ASSERT_ERR(first == left + 1 && second == right + 1);
        SOFT_ASSERT_ERR(owners[left] == id && owners[right] == id);
        owners[left] = owners[right] = 0;
      }
    }

    /* берем слудущую строку*/
    line = strtok(NULL, "\n");
  }

  /* проверяем что каждая вилка свободна */
  for (int i = 0; i < count; i++) {
    SOFT_ASSERT_ERR(owners[i] == 0);
  }

  free(owners);
  free(copy);
}

static void test_help(void) {
  char *args[] = {binary, "--help", NULL};
  char *output = run_program(args, 0, 0);
  check_text(output, "Использование");
  free(output);
}

/* базовый тест */
static void test_normal(void) {
  char *args[] = {binary, "2", "1", "3", "3", "7", "7", "7", "0", "42", NULL};
  char *output = run_program(args, 0, 1);
  check_text(output, "Модельное время: 17 ms");
  check_text(output, "Всего завершённых приёмов пищи: 2\n");
  check_text(output, "Среднее ожидание перед начатой едой: 3.50 ms");
  check_text(output, "P1: циклов=1/1");
  check_text(output, "P2: циклов=1/1");
  check_forks(output, 2);
  free(output);
}

static void test_several_cycles(void) {
  char *args[] = {binary, "5", "3", "0", "30", "1", "20", "1000", "0", "42", NULL};
  char *output = run_program(args, 0, 1);
  check_text(output, "все философы выполнили заданное число циклов");
  check_text(output, "Всего завершённых приёмов пищи: 15\n");

  for (int i = 1; i <= 5; i++) {
    char text[64];
    snprintf(text, sizeof(text), "P%d: циклов=3/3", i);
    check_text(output, text);
  }

  check_forks(output, 5);
  /* при одинаковом seed результат должен повторяться */
  char *second = run_program(args, 0, 1);
  SOFT_ASSERT_ERR(output != NULL && second != NULL && strcmp(output, second) == 0);

  free(output);
  free(second);
}

/* тест на лимит ожидания (сразу 2 и на не вход в лимит и на проверку границы)*/
static void test_wait_limit(void) {
  char *args[] = {binary, "2", "1", "0", "0", "200", "200", "150", "0", "42", NULL};
  char *output = run_program(args, 2, 1);
  check_text(output, "философ 2 ждёт 151 ms");
  check_text(output, "Всего завершённых приёмов пищи: 0\n");
  check_forks(output, 2);
  free(output);

  /* ожидание ровно до лимита разрешено  */
  args[7] = "200";
  output = run_program(args, 0, 1);
  check_text(output, "Всего завершённых приёмов пищи: 2\n");
  check_forks(output, 2);
  free(output);
}

/* общий лимит времени*/
static void test_time_limit(void) {
  char *args[] = {binary, "2", "1", "0", "0", "7", "7", "1000", "0", "42", "fifo", "5", NULL};
  char *output = run_program(args, 2, 1);
  check_text(output, "достигнут общий лимит");
  check_text(output, "Модельное время: 5 ms");
  check_forks(output, 2);
  free(output);

  /* все циклы заканчиваются ровно на границе */
  args[11] = "14";
  output = run_program(args, 0, 1);
  check_text(output, "Модельное время: 14 ms");
  check_forks(output, 2);
  free(output);
}

/* неправильные параметры */
static void test_bad_parameters(void) {
  char *bad_numbers[] = {"abc", "1", "-2", "2147483648"};

  for (int i = 0; i < 4; i++) {
    char *args[] = {binary, bad_numbers[i], NULL};
    char *output = run_program(args, 1, 0);
    check_text(output, "Ошибка:");
    free(output);
  }

  char *args[] = {binary, "2", "1", "10", "5", NULL};
  char *output = run_program(args, 1, 0);
  check_text(output, "некорректные параметры");
  free(output);

  char *strategy[] = {binary, "2", "1", "0", "0", "1", "1", "1000", "0", "42", "unknown", NULL};
  output = run_program(strategy, 1, 0);
  check_text(output, "поддерживается только стратегия fifo");
  free(output);
}

/* При остановке сигналом еда прерывается, но вилки освобождаются. */
static void test_signals(void) {
  int signals[] = {SIGINT, SIGTERM};
  for (int i = 0; i < 2; i++) {
    char *args[] = {binary, "2", "100", "0", "0", "200", "200",
                    "1000", "10000", "42", "fifo", "0", NULL};
    char *output = run_program_with_signal(args, 128 + signals[i], 1, signals[i]);
    check_text(output, "прерывание пользователем (SIGINT/SIGTERM)");
    check_text(output, "еда прервана");
    check_text(output, "Всего завершённых приёмов пищи: 0\n");
    check_forks(output, 2);
    free(output);
  }
}

/* стресс-тест */
static void test_many_philosophers(void) {
  char *args[] = {binary, "137", "1", "0", "0", "1", "1",
                  "1000", "0", "42", NULL};
  char *output = run_program(args, 0, 1);
  check_text(output, "все философы выполнили заданное число циклов");
  check_text(output, "Всего завершённых приёмов пищи: 137\n");

  for (int i = 1; i <= 137; i++) {
    char text[64];
    snprintf(text, sizeof(text), "P%d: циклов=1/1", i);
    check_text(output, text);
  }
  
  check_forks(output, 137);
  free(output);
}

/* проверка работы именно очереди */
static void test_fifo(void) {
  char *args[] = {binary, "3", "2", "0", "0", "7", "7",
                  "1000", "0", "42", NULL};
  char *output = run_program(args, 0, 1);
  check_text(output, "Всего завершённых приёмов пищи: 6\n");

  if (output != NULL) {
    char *copy = strdup(output);
    SOFT_ASSERT_ERR(copy != NULL);

    if (copy != NULL) {
      int meals = 0;
      char *line = strtok(copy, "\n");

      while (line != NULL) {
        long long time;
        int id, offset = 0;

        sscanf(line, "[%lld ms] Философ %d: %n", &time, &id, &offset);
        if (offset > 0 &&
            strncmp(line + offset, "начал есть", strlen("начал есть")) == 0) {
          /* ожидаем 1, 2, 3, 1, 2, 3 */
          SOFT_ASSERT_ERR(id == meals % 3 + 1);
          meals++;
        }

        line = strtok(NULL, "\n");
      }
      SOFT_ASSERT_ERR(meals == 6);
      free(copy);
    }
  }
  
  check_forks(output, 3);
  free(output);
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "Использование: %s путь_к_программе\n", argv[0]);
    return 1;
  }

  /* если передали путь, то копируем его в binary, иначе создаем буффер для текущего каталога*/
  int length;
  if (argv[1][0] == '/') {
    length = snprintf(binary, sizeof(binary), "%s", argv[1]);
  } else {
    char directory[PATH_MAX];
    if (getcwd(directory, sizeof(directory)) == NULL) {
      perror("getcwd");
      return 1;
    }

    /* соединяем пути */
    length = snprintf(binary, sizeof(binary), "%s/%s", directory, argv[1]);
  }

  SOFT_ASSERT_ERR(length >= 0 && (size_t)length < sizeof(binary));
  SOFT_ASSERT_ERR(access(binary, X_OK) == 0);
  if (fl_err != 0) {
    return 1;
  }

  test_help();
  test_normal();
  test_several_cycles();
  test_wait_limit();
  test_time_limit();
  test_bad_parameters();
  test_signals();
  test_many_philosophers();
  test_fifo();

  if (fl_err != 0) {
    fprintf(stderr, "Не пройдено проверок: %d\n", fl_err);
    return 1;
  }
  puts("Все тесты пройдены (9 групп проверок).");
  return 0;
}
