#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <linux/filter.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <uapi/linux/bpf.h>

#include "ipft.h"

int
list_functions(struct ipft_tracer_opt *opt)
{
  int error;
  struct ipft_regex *re;
  struct ipft_symsdb *sdb;
  struct ipft_sym *sym, **syms;

  error = regex_create(&re, opt->regex);
  if (error == -1) {
    ERROR("regex_create failed\n");
    return -1;
  }

  struct ipft_symsdb_opt sdb_opt = {
      .max_args = get_max_args_for_backend(opt->backend),
      .max_skb_pos = get_max_skb_pos_for_backend(opt->backend),
  };

  error = symsdb_create(&sdb, &sdb_opt);
  if (error == -1) {
    ERROR("Failed to initialize symsdb\n");
    return -1;
  }

  fprintf(stderr, "%64.64s\t%18s\t%s\n", "NAME", "ADDR", "SKB_POSITION");

  for (int i = 0; i < sdb_opt.max_skb_pos; i++) {
    syms = symsdb_get_syms_by_pos(sdb, i);
    if (syms == NULL) {
      continue;
    }
    for (int j = 0; j < symsdb_get_syms_total_by_pos(sdb, i); j++) {
      sym = syms[j];
      if (regex_match(re, sym->symname)) {
        printf("%64.64s\t0x%016lx\t%d\n", sym->symname, sym->addr, i);
      }
    }
  }

  return 0;
}

int
probe_kprobe_multi(void)
{
  int fd;

  struct bpf_insn insns[] = {
      BPF_MOV64_IMM(BPF_REG_0, 0),
      BPF_EXIT_INSN(),
  };

  struct bpf_prog_load_opts popts = {
      .sz = sizeof(popts),
      .expected_attach_type = BPF_TRACE_KPROBE_MULTI,
  };

  /*
   * Actually, this load always succeeds regardless of the kernel support of
   * BPF_PROG_TYPE_KPROBE, because kernel doesn't check expected_attach_type
   * for BPF_PROG_TYPE_KPROBE. Thus, we need to attach program to probe support.
   */
  fd = bpf_prog_load(BPF_PROG_TYPE_KPROBE, NULL, "GPL", insns, 2, &popts);
  if (fd < 0) {
    return 0;
  }

  const char *syms[] = {"__kfree_skb"};

  struct bpf_link_create_opts lopts = {
      .sz = sizeof(lopts),
      .kprobe_multi =
          {
              .cnt = 1,
              .syms = syms,
          },
  };

  fd = bpf_link_create(fd, 0, BPF_TRACE_KPROBE_MULTI, &lopts);
  if (fd < 0) {
    return 0;
  }

  return 1;
}

char *
libbpf_error_string(int error)
{
  static char buf[4096]; // not thread safe
  memset(buf, 0, sizeof(buf));
  libbpf_strerror(error, buf, sizeof(buf));
  return buf;
}
