AC_INIT([lightplay], [devel], [juhaerk@icloud.com])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AC_PROG_CC
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([
 Makefile
 src/Makefile
])

AC_CHECK_FUNC([pledge])

AC_SEARCH_LIBS([mergesort], [bsd], [], [
  AC_MSG_ERROR([unable to find the mergesort() function])
])

AC_SEARCH_LIBS([mio_open], [sndio], [], [
  AC_MSG_ERROR([unable to find the mio_open() function])
])

if test "$ac_cv_search_mergesort" = "-lbsd"; then
  AC_DEFINE([HAVE_MERGESORT_IN_LIBBSD], [1], [mergesort in libbsd])
fi

if test "$ac_cv_func_pledge" = "yes"; then
  AC_DEFINE([HAVE_PLEDGE], [1], [pledge])
fi

AC_OUTPUT
