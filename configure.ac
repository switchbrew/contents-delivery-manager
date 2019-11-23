#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.61)
AC_INIT([contents-delivery-manager],[1.0.0],[https://github.com/switchbrew/contents-delivery-manager/issues])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_SRCDIR([source/manager.c])

AM_INIT_AUTOMAKE([subdir-objects foreign])

AC_CANONICAL_BUILD
AC_CANONICAL_HOST

AC_PROG_CC

AX_APPEND_COMPILE_FLAGS([-pthread])

AC_SYS_LARGEFILE

NET_LIBS=""

case "$host" in
   *-*-mingw*)
      NET_LIBS="-lws2_32"
      CFLAGS="$CFLAGS -D__USE_MINGW_ANSI_STDIO -D_WIN32_WINNT=0x0600"
      ;;
esac

CFLAGS="$CFLAGS -std=gnu99"

AC_SUBST(NET_LIBS)
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
