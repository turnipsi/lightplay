AC_INIT([lightplay], [1.0], [juhaerk@icloud.com])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AC_PROG_CC
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([
 Makefile
 src/Makefile
])

AC_SEARCH_LIBS([mergesort], [bsd], [], [
  AC_MSG_ERROR([unable to find the mergesort() function])
])

AC_SEARCH_LIBS([mio_open], [sndio], [], [
  AC_MSG_ERROR([unable to find the mio_open() function])
])

AC_SEARCH_LIBS([pledge], [], [
  AC_DEFINE([HAVE_PLEDGE], [1], [pledge])
], [])

AC_SEARCH_LIBS([reallocarray], [bsd], [], [
  AC_MSG_ERROR([unable to find the reallocarray() function])
])

AC_OUTPUT
