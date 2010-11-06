/*
 * Copyright (C) 2007-2010 Andre Noll <maan@systemlinux.org>
 *
 * Licensed under the GPL v2. For licencing details see COPYING.
 */

/** \file signal.h exported symbols from signal.c */

int signal_init(void);
int install_sighandler(int);
int next_signal(void);
void signal_shutdown(void);
int reap_child(pid_t *pid, int *status);
