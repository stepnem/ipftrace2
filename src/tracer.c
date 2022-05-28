#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <linux/types.h>

#include <gelf.h>
#include <libelf.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "ipft.h"
#include "ipft_kprobe.bpf.o.h"
#include "ipft_kprobe_multi.bpf.o.h"
#include "ipft_ftrace.bpf.o.h"
#include "null_module.bpf.o.h"

struct ipft_tracer {
  struct bpf_object *bpf;
  struct ipft_regex *re;
  struct ipft_symsdb *sdb;
  struct ipft_tracer_opt *opt;
  struct ipft_output *out;
  struct ipft_script *script;
  struct perf_buffer *pb;
};

static struct {
  size_t total;
  size_t succeeded;
  size_t failed;
  size_t filtered;
} attach_stat = {0};

static int
attach_cb(const char *sym, struct ipft_syminfo *si, void *data)
{
  char name[32] = {0};
  struct bpf_link *link;
  struct bpf_program *prog;
  struct ipft_tracer *t = (struct ipft_tracer *)data;

  if (!regex_match(t->re, sym)) {
    attach_stat.filtered++;
    return 0;
  }

  if (sprintf(name, "ipft_main%d", si->skb_pos) < 0) {
    fprintf(stderr, "sprintf failed\n");
    return -1;
  }

  prog = bpf_object__find_program_by_name(t->bpf, name);
  if (prog == NULL) {
    fprintf(stderr, "bpf_object__find_program_by_name failed\n");
    return -1;
  }

  link = bpf_program__attach_kprobe(prog, false, sym);
  if (link == NULL) {
    attach_stat.failed++;
    fprintf(stderr, "Attach kprobe failed for %s\n", sym);
    return -1;
  }

  attach_stat.succeeded++;

  fprintf(
      stderr,
      "\rAttaching program (total %zu, succeeded %zu, failed %zu, filtered: "
      "%zu)",
      attach_stat.total, attach_stat.succeeded, attach_stat.failed,
      attach_stat.filtered);
  fflush(stderr);

  return 0;
}

static int
attach_kprobe(struct ipft_tracer *t)
{
  int error;

  error = symsdb_sym2info_foreach(t->sdb, attach_cb, t);
  if (error == -1) {
    return -1;
  }

  return 0;
}

static int
attach_kprobe_multi(struct ipft_tracer *t)
{
  int error;

  for (int i = 0; i < MAX_SKB_POS; i++) {
    size_t cur = 0;
    const char **syms;
    char name[32] = {0};
    struct bpf_link *link;
    struct bpf_program *prog;

    if (sprintf(name, "ipft_main%d", i) < 0) {
      fprintf(stderr, "sprintf failed\n");
      return -1;
    }

    prog = bpf_object__find_program_by_name(t->bpf, name);
    if (prog == NULL) {
      fprintf(stderr, "bpf_object__find_program_by_name failed\n");
      return -1;
    }

    memset(name, 0, sizeof(name));

    syms = calloc(symsdb_get_pos2syms_total(t->sdb, i), sizeof(char *));
    if (syms == NULL) {
      fprintf(stderr, "calloc failed\n");
      return -1;
    }

    for (int j = 0; j < symsdb_get_pos2syms_total(t->sdb, i); j++) {
      const char *sym = symsdb_pos2syms_get(t->sdb, i, j);

      if (!regex_match(t->re, sym)) {
        attach_stat.filtered++;
        continue;
      }

      syms[cur++] = sym;
    }

    struct bpf_kprobe_multi_opts opts = {
        .sz = sizeof(opts),
        .syms = syms,
        .cnt = cur,
    };

    link = bpf_program__attach_kprobe_multi_opts(prog, NULL, &opts);

    error = libbpf_get_error(link);
    if (error != 0) {
      char errbuf[256] = {0};
      libbpf_strerror(error, errbuf, 256);
      fprintf(stderr, "bpf_program__attach_kprobe_multi_opts failed: %s\n",
              errbuf);
      attach_stat.failed += opts.cnt;
    } else {
      attach_stat.succeeded += opts.cnt;
    }

    fprintf(
        stderr,
        "\rAttaching program (total %zu, succeeded %zu, failed %zu filtered: "
        "%zu)",
        attach_stat.total, attach_stat.succeeded, attach_stat.failed,
        attach_stat.filtered);
    fflush(stderr);
  }

  return 0;
}

static int
attach_ftrace(struct ipft_tracer *t)
{
  int error, btf_fd;
  char log_buf[4096] = {0};

  btf_fd = bpf_object__btf_fd(t->bpf);
  if (btf_fd < 0) {
    fprintf(stderr, "bpf_object__btf_fd failed\n");
    return -1;
  }

  for (int i = 0; i < MAX_SKB_POS; i++) {
    char name[32];
    int entry_fd, exit_fd;
    size_t entry_size, exit_size;
    struct bpf_program *entry_prog, *exit_prog;
    const struct bpf_insn *entry_insns, *exit_insns;

    memset(name, 0, sizeof(name));

    if (sprintf(name, "ipft_main%d", i) < 0) {
      fprintf(stderr, "sprintf failed\n");
      return -1;
    }

    entry_prog = bpf_object__find_program_by_name(t->bpf, name);
    if (entry_prog == NULL) {
      fprintf(stderr, "bpf_object__find_program_by_name failed\n");
      return -1;
    }

    memset(name, 0, sizeof(name));

    if (sprintf(name, "ipft_main_return%d", i) < 0) {
      fprintf(stderr, "sprintf failed\n");
      return -1;
    }

    exit_prog = bpf_object__find_program_by_name(t->bpf, name);
    if (exit_prog == NULL) {
      fprintf(stderr, "bpf_object__find_program_by_name failed\n");
      return -1;
    }

    entry_insns = bpf_program__insns(entry_prog);
    exit_insns = bpf_program__insns(exit_prog);
    entry_size = bpf_program__insn_cnt(entry_prog);
    exit_size = bpf_program__insn_cnt(exit_prog);

    for (int j = 0; j < symsdb_get_pos2syms_total(t->sdb, i); j++) {
      struct ipft_syminfo *sinfo;
      int entry_tp_fd, exit_tp_fd;
      struct bpf_prog_load_opts opts = {0};
      const char *sym = symsdb_pos2syms_get(t->sdb, i, j);

      if (!regex_match(t->re, sym)) {
        attach_stat.filtered++;
        continue;
      }

      error = symsdb_get_sym2info(t->sdb, sym, &sinfo);
      if (error != 0) {
        fprintf(stderr, "symsdb_get_sym2info failed\n");
        return -1;
      }

      opts.sz = sizeof(opts), opts.prog_btf_fd = btf_fd,
      opts.attach_btf_id = sinfo->btf_id,
      opts.attach_btf_obj_fd = sinfo->btf_fd, opts.log_level = 4,
      opts.log_size = 4096, opts.log_buf = log_buf,

      opts.expected_attach_type = BPF_TRACE_FENTRY;

      entry_fd = bpf_prog_load(BPF_PROG_TYPE_TRACING, NULL, "GPL", entry_insns,
                               entry_size, &opts);
      if (error == -1) {
        fprintf(stderr, "bpf_prog_load for %s entry failed\n%s", sym, log_buf);
        return -1;
      }

      opts.expected_attach_type = BPF_TRACE_FEXIT;

      exit_fd = bpf_prog_load(BPF_PROG_TYPE_TRACING, NULL, "GPL", exit_insns,
                              exit_size, &opts);
      if (error == -1) {
        fprintf(stderr, "bpf_prog_load for %s exit failed\n%s", sym, log_buf);
        return -1;
      }

      entry_tp_fd = bpf_raw_tracepoint_open(NULL, entry_fd);
      if (entry_tp_fd < 0) {
        fprintf(stderr, "bpf_raw_tracepoint_open for %s entry failed: %s\n",
                sym, strerror(errno));
        return -1;
      }

      exit_tp_fd = bpf_raw_tracepoint_open(NULL, exit_fd);
      if (exit_tp_fd < 0) {
        fprintf(stderr, "bpf_raw_tracepoint_open for %s exit failed: %s\n", sym,
                strerror(errno));
        return -1;
      }

      attach_stat.succeeded++;

      fprintf(
          stderr,
          "\rAttaching program (total %zu, succeeded %zu, failed %zu filtered: "
          "%zu)",
          attach_stat.total, attach_stat.succeeded, attach_stat.failed,
          attach_stat.filtered);
      fflush(stderr);
    }
  }

  return 0;
}

static int
attach_all(struct ipft_tracer *t)
{
  int error;

  attach_stat.total = symsdb_get_sym2info_total(t->sdb);

  if (strcmp(t->opt->backend, "kprobe") == 0) {
    error = attach_kprobe(t);
    if (error == -1) {
      return -1;
    }
  } else if (strcmp(t->opt->backend, "kprobe-multi") == 0) {
    error = attach_kprobe_multi(t);
    if (error == -1) {
      return -1;
    }
  } else if (strcmp(t->opt->backend, "ftrace") == 0) {
    error = attach_ftrace(t);
    if (error == -1) {
      return -1;
    }
  } else {
    fprintf(stderr, "Unsupported backend %s\n", t->opt->backend);
    return -1;
  }

  fprintf(stderr, "\n");

  return error;
}

struct perf_sample_data {
  struct perf_event_header header;
  uint32_t size;
  uint8_t data[0];
};

static enum bpf_perf_event_ret
trace_cb(void *ctx, __unused int cpu, struct perf_event_header *ehdr)
{
  int error;
  struct ipft_tracer *t = (struct ipft_tracer *)ctx;
  struct perf_sample_data *s = (struct perf_sample_data *)ehdr;

  switch (ehdr->type) {
  case PERF_RECORD_SAMPLE:
    error = output_on_trace(t->out, (struct ipft_event *)s->data);
    if (error == -1) {
      return LIBBPF_PERF_EVENT_ERROR;
    }
    break;
  case PERF_RECORD_LOST:
    error = 0;
    break;
  default:
    fprintf(stderr, "BUG: Unknown event type %d\n", ehdr->type);
    return LIBBPF_PERF_EVENT_ERROR;
  }

  return LIBBPF_PERF_EVENT_CONT;
}

static int
perf_buffer_create(struct perf_buffer **pbp, struct ipft_tracer *t,
                   size_t perf_page_cnt, uint64_t perf_sample_period,
                   uint32_t perf_wakeup_events)
{
  struct perf_buffer *pb;

  struct perf_buffer_raw_opts pb_opts = {
      .sz = sizeof(pb_opts),
      .cpu_cnt = 0,
  };

  struct perf_event_attr pe_attr = {
      .type = PERF_TYPE_SOFTWARE,
      .config = PERF_COUNT_SW_BPF_OUTPUT,
      .sample_period = perf_sample_period,
      .sample_type = PERF_SAMPLE_RAW,
      .wakeup_events = perf_wakeup_events,
  };

  pb = perf_buffer__new_raw(bpf_object__find_map_fd_by_name(t->bpf, "events"),
                            perf_page_cnt, &pe_attr, trace_cb, t, &pb_opts);
  if (pb == NULL) {
    fprintf(stderr, "perf_buffer__new_raw failed\n");
    return -1;
  }

  *pbp = pb;

  return 0;
}

/*
 * We need to carefully make sure tmpfiles we make in below functions
 * are unlinked. Otherwise, we'll leak the tmpfiles in user's /tmp.
 */

static int
create_tmpfile_from_image(int *fdp, char **namep, uint8_t *image,
                          size_t image_size)
{
  int fd;
  char *name;

  name = strdup("/tmp/ipft_XXXXXX");
  if (name == NULL) {
    fprintf(stderr, "Failed to allocate memory for tmpfile name\n");
    return -1;
  }

  fd = mkstemp(name);
  if (fd == -1) {
    fprintf(stderr, "Failed to create tmpfile\n");
    return -1;
  }

  if (write(fd, image, image_size) == -1) {
    fprintf(stderr, "Failed to write image to tmpfile\n");
    goto err0;
  }

  *fdp = fd;
  *namep = name;

  return 0;

err0:
  close(fd);
  unlink(name);
  return -1;
}

static int
do_link(char **namep, uint8_t *target_image, size_t target_image_size,
        uint8_t *module_image, size_t module_image_size)
{
  char *name;
  struct bpf_linker *linker;
  char *target_name, *module_name;
  int error = -1, target_fd, module_fd;

  error = create_tmpfile_from_image(&target_fd, &target_name, target_image,
                                    target_image_size);
  if (error == -1) {
    fprintf(stderr, "create_tmpfile_from_image for target image failed\n");
    return -1;
  }

  error = create_tmpfile_from_image(&module_fd, &module_name, module_image,
                                    module_image_size);
  if (error == -1) {
    fprintf(stderr, "create_tmpfile_from_image for module image failed\n");
    goto err0;
  }

  struct bpf_linker_opts lopts = {.sz = sizeof(lopts)};

  name = tmpnam(NULL);

  linker = bpf_linker__new(name, &lopts);
  if (linker == NULL) {
    fprintf(stderr, "bpf_linker__create failed\n");
    goto err1;
  }

  struct bpf_linker_file_opts fopts = {.sz = sizeof(fopts)};

  error = bpf_linker__add_file(linker, target_name, &fopts);
  if (error == -1) {
    fprintf(stderr, "bpf_linker__add_file failed\n");
    goto err2;
  }

  error = bpf_linker__add_file(linker, module_name, &fopts);
  if (error == -1) {
    fprintf(stderr, "bpf_linker__add_file failed\n");
    goto err2;
  }

  error = bpf_linker__finalize(linker);
  if (error == -1) {
    fprintf(stderr, "bpf_linker__finalize failed\n");
    goto err2;
  }

  error = 0;
  *namep = name;

err2:
  bpf_linker__free(linker);
err1:
  close(module_fd);
  unlink(module_name);
err0:
  close(target_fd);
  unlink(target_name);
  return error;
}

static int
get_target_image(char *backend, uint8_t **imagep, size_t *image_sizep)
{
  if (strcmp(backend, "kprobe") == 0) {
    *imagep = ipft_kprobe_bpf_o;
    *image_sizep = ipft_kprobe_bpf_o_len;
  } else if (strcmp(backend, "kprobe-multi") == 0) {
    *imagep = ipft_kprobe_multi_bpf_o;
    *image_sizep = ipft_kprobe_multi_bpf_o_len;
  } else if (strcmp(backend, "ftrace") == 0) {
    *imagep = ipft_ftrace_bpf_o;
    *image_sizep = ipft_ftrace_bpf_o_len;
  } else {
    fprintf(stderr, "Unsupported backend %s\n", backend);
    return -1;
  }
  return 0;
}

static int
get_default_module_image(uint8_t **imagep, size_t *image_sizep)
{
  *imagep = null_module_bpf_o;
  *image_sizep = null_module_bpf_o_len;
  return 0;
}

static int
ftrace_set_init_target(struct bpf_object *bpf, struct ipft_tracer *t)
{
  int error;
  char name[32];

  for (int i = 0; i < MAX_SKB_POS; i++) {
    const char *sym;
    struct bpf_program *entry_prog, *exit_prog;

    memset(name, 0, sizeof(name));

    if (sprintf(name, "ipft_main%d", i) < 0) {
      fprintf(stderr, "sprintf failed\n");
      return -1;
    }

    entry_prog = bpf_object__find_program_by_name(bpf, name);
    if (entry_prog == NULL) {
      fprintf(stderr, "bpf__find_program_by_name failed\n");
      return -1;
    }

    memset(name, 0, sizeof(name));

    if (sprintf(name, "ipft_main_return%d", i) < 0) {
      fprintf(stderr, "sprintf failed\n");
      return -1;
    }

    exit_prog = bpf_object__find_program_by_name(bpf, name);
    if (exit_prog == NULL) {
      fprintf(stderr, "bpf__find_program_by_name failed\n");
      return -1;
    }

    sym = symsdb_pos2syms_get(t->sdb, i, 0);
    if (sym == NULL) {
      bpf_program__set_autoload(entry_prog, false);
      bpf_program__set_autoload(exit_prog, false);
      continue;
    }

    error = bpf_program__set_attach_target(entry_prog, 0, sym);
    if (error == -1) {
      fprintf(stderr, "bpf_program__set_attach_target failed\n");
      return -1;
    }

    error = bpf_program__set_attach_target(exit_prog, 0, sym);
    if (error == -1) {
      fprintf(stderr, "bpf_program__set_attach_target failed\n");
      return -1;
    }
  }

  return 0;
}

static int
bpf_create(struct bpf_object **bpfp, uint32_t mark, uint32_t mask,
           char *backend, struct ipft_tracer *t)
{
  int error;
  char *name;
  struct bpf_object *bpf;
  struct ipft_trace_config conf;
  uint8_t *target_image, *module_image;
  size_t target_image_size, module_image_size;

  error = get_target_image(backend, &target_image, &target_image_size);
  if (error != 0) {
    fprintf(stderr, "get_target_image failed\n");
    return -1;
  }

  if (t->script != NULL) {
    error = script_get_program(t->script, &module_image, &module_image_size);
    if (error != 0) {
      fprintf(stderr, "script_get_program failed\n");
      return -1;
    }
  } else {
    error = get_default_module_image(&module_image, &module_image_size);
    if (error != 0) {
      fprintf(stderr, "get_default_module failed\n");
      return -1;
    }
  }

  error = do_link(&name, target_image, target_image_size, module_image,
                  module_image_size);
  if (error == -1) {
    fprintf(stderr, "do_link failed\n");
    return -1;
  }

  struct bpf_object_open_opts opts = {
      .sz = sizeof(opts),
      .object_name = "ipft",
  };

  bpf = bpf_object__open(name);
  if (bpf == NULL) {
    fprintf(stderr, "bpf_object__open failed\n");
    return -1;
  }

  unlink(name);

  if (strcmp(backend, "ftrace") == 0) {
    error = ftrace_set_init_target(bpf, t);
    if (error == -1) {
      fprintf(stderr, "ftrace_setup_prep failed\n");
      return -1;
    }
  }

  error = bpf_object__load(bpf);
  if (error == -1) {
    fprintf(stderr, "bpf_object__load failed\n");
    return -1;
  }

  conf.mark = mark;
  conf.mask = mask;

  error = bpf_map_update_elem(bpf_object__find_map_fd_by_name(bpf, "config"),
                              &(int){0}, &conf, 0);
  if (error == -1) {
    fprintf(stderr, "Cannot update config map\n");
    return -1;
  }

  *bpfp = bpf;

  return 0;
}

static bool end = false;

static void
handle_signal(__unused int signum)
{
  end = true;
  signal(SIGINT, SIG_DFL);
  signal(SIGTERM, SIG_DFL);
}

static void *
handle_tcp_probe(void *arg)
{
  int error, lsock, csock;
  struct ipft_tracer_opt *opt = (struct ipft_tracer_opt *)arg;

  lsock = socket(AF_INET, SOCK_STREAM, 0);
  if (lsock == -1) {
    fprintf(stderr, "socket failed: %s\n", strerror(errno));
    pthread_exit(arg);
  }

  error = setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
  if (error == -1) {
    fprintf(stderr, "setsockopt failed: %s\n", strerror(errno));
    pthread_exit(arg);
  }

  struct sockaddr_in laddr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = inet_addr("0.0.0.0"),
      .sin_port = htons(opt->probe_server_port),
  };

  error = bind(lsock, (struct sockaddr *)&laddr, sizeof(laddr));
  if (error == -1) {
    fprintf(stderr, "bind failed: %s\n", strerror(errno));
    pthread_exit(arg);
  }

  error = listen(lsock, 100);
  if (error == -1) {
    fprintf(stderr, "listen failed: %s\n", strerror(errno));
    pthread_exit(arg);
  }

  while (!end) {
    struct sockaddr_in caddr = {};
    socklen_t caddr_len = sizeof(caddr);

    csock = accept(lsock, (struct sockaddr *)&caddr, &caddr_len);
    if (csock == -1) {
      fprintf(stderr, "accept failed: %s\n", strerror(errno));
      continue;
    }

    close(csock);
  }

  close(lsock);

  return arg;
}

int
tracer_run(struct ipft_tracer *t)
{
  int error;
  pthread_t thread;

  error = attach_all(t);
  if (error) {
    fprintf(stderr, "attach_all failed\n");
    return -1;
  }

  fprintf(stderr, "Trace ready!\n");

  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);

  if (t->opt->enable_probe_server) {
    error = pthread_create(&thread, NULL, handle_tcp_probe, (void *)t->opt);
    if (error == -1) {
      fprintf(stderr, "pthread_create failed: %s\n", strerror(errno));
      return -1;
    }

    error = pthread_detach(thread);
    if (error == -1) {
      fprintf(stderr, "pthread_detach failed: %s\n", strerror(errno));
      return -1;
    }
  }

  while (!end) {
    if ((error = perf_buffer__poll(t->pb, 1000)) < 0) {
      /* perf_buffer__poll cancelled with signal */
      if (end) {
        break;
      }
      return -1;
    }
  }

  error = output_post_trace(t->out);
  if (error == -1) {
    fprintf(stderr, "output_post_trace failed\n");
    return -1;
  }

  if (t->script != NULL) {
    script_exec_fini(t->script);
  }

  return 0;
}

int
tracer_create(struct ipft_tracer **tp, struct ipft_tracer_opt *opt)
{
  int error;
  struct ipft_tracer *t;

  t = calloc(1, sizeof(*t));
  if (t == NULL) {
    fprintf(stderr, "Failed to allocate memory\n");
    return -1;
  }

  t->opt = opt;

  error = symsdb_create(&t->sdb);
  if (error != 0) {
    fprintf(stderr, "symsdb_create failed\n");
    return -1;
  }

  error = script_create(&t->script, opt->script);
  if (error == -1) {
    fprintf(stderr, "script_create failed\n");
    return -1;
  }

  error = bpf_create(&t->bpf, opt->mark, opt->mask, opt->backend, t);
  if (error == -1) {
    fprintf(stderr, "bpf_create failed\n");
    return -1;
  }

  error = regex_create(&t->re, opt->regex);
  if (error != 0) {
    fprintf(stderr, "regex_create failed\n");
    return -1;
  }

  error =
      output_create(&t->out, opt->output_type, t->sdb, t->script, opt->tracer);
  if (error != 0) {
    fprintf(stderr, "output_create failed\n");
    return -1;
  }

  error = perf_buffer_create(&t->pb, t, opt->perf_page_cnt,
                             opt->perf_sample_period, opt->perf_wakeup_events);
  if (error == -1) {
    fprintf(stderr, "perf_buffer_create failed\n");
    return -1;
  }

  *tp = t;

  return 0;
}
