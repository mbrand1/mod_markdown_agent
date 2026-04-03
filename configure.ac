dnl configure.ac for mod_markdown_agent
dnl Run: autoreconf -i && ./configure && make && make install

AC_INIT([mod_markdown_agent], [0.1.0], [brandon@netsville.com])
AC_CONFIG_SRCDIR([mod_markdown_agent.c])

dnl --- C compiler ---
AC_PROG_CC

dnl --- Find apxs ---
AC_ARG_WITH([apxs],
  [AS_HELP_STRING([--with-apxs=PATH], [path to Apache apxs binary])],
  [APXS="$withval"],
  [APXS=""])

if test -z "$APXS"; then
  AC_MSG_CHECKING([for apxs])
  for dir in /usr/bin /usr/sbin /usr/local/bin /usr/local/apache2/bin /usr/local/apache/bin; do
    for name in apxs apxs2; do
      if test -x "$dir/$name"; then
        APXS="$dir/$name"
        break 2
      fi
    done
  done
  if test -n "$APXS"; then
    AC_MSG_RESULT([$APXS])
  else
    AC_MSG_RESULT([not found])
    AC_MSG_ERROR([apxs not found. Install apache2-dev (Debian/Ubuntu) or httpd-devel (RHEL/CentOS), or use --with-apxs=PATH])
  fi
else
  if test ! -x "$APXS"; then
    AC_MSG_ERROR([--with-apxs=$APXS: not found or not executable])
  fi
fi

AC_SUBST([APXS])

dnl Get apachectl path from the same prefix as apxs
APXS_DIR=`dirname "$APXS"`
APACHECTL="$APXS_DIR/apachectl"
if test ! -x "$APACHECTL"; then
  dnl Debian puts apachectl elsewhere
  APACHECTL=`which apachectl 2>/dev/null || echo "apachectl"`
fi
AC_SUBST([APACHECTL])

dnl --- Find libxml2 ---
AC_ARG_WITH([libxml2],
  [AS_HELP_STRING([--with-libxml2=PATH], [path to libxml2 include directory])],
  [LIBXML2_INC="$withval"],
  [LIBXML2_INC=""])

dnl Try pkg-config first
if test -z "$LIBXML2_INC"; then
  PKG_CHECK_EXISTS([libxml-2.0], [
    LIBXML2_INC=`pkg-config --variable=includedir libxml-2.0`/libxml2
    LIBXML2_LIBS=`pkg-config --libs libxml-2.0`
  ], [
    dnl Try xml2-config
    AC_MSG_CHECKING([for xml2-config])
    XML2_CONFIG=`which xml2-config 2>/dev/null`
    if test -n "$XML2_CONFIG"; then
      AC_MSG_RESULT([$XML2_CONFIG])
      LIBXML2_INC=`$XML2_CONFIG --cflags | sed 's/^-I//'`
      LIBXML2_LIBS=`$XML2_CONFIG --libs`
    else
      AC_MSG_RESULT([not found])
      dnl Fall back to common paths
      for dir in /usr/include/libxml2 /usr/local/include/libxml2; do
        if test -d "$dir"; then
          LIBXML2_INC="$dir"
          break
        fi
      done
    fi
  ])
fi

if test -z "$LIBXML2_INC"; then
  AC_MSG_ERROR([libxml2 headers not found. Install libxml2-dev (Debian/Ubuntu) or libxml2-devel (RHEL/CentOS), or use --with-libxml2=PATH])
fi

if test ! -d "$LIBXML2_INC"; then
  AC_MSG_ERROR([libxml2 include directory not found: $LIBXML2_INC])
fi

dnl Default libs if not set by pkg-config/xml2-config
if test -z "$LIBXML2_LIBS"; then
  LIBXML2_LIBS="-lxml2"
fi

AC_SUBST([LIBXML2_INC])
AC_SUBST([LIBXML2_LIBS])

dnl Verify the header is actually there
saved_CPPFLAGS="$CPPFLAGS"
CPPFLAGS="$CPPFLAGS -I$LIBXML2_INC"
AC_CHECK_HEADER([libxml/HTMLparser.h], [],
  [AC_MSG_ERROR([libxml/HTMLparser.h not found in $LIBXML2_INC])])
CPPFLAGS="$saved_CPPFLAGS"

dnl --- Summary ---
AC_MSG_NOTICE([])
AC_MSG_NOTICE([  mod_markdown_agent $PACKAGE_VERSION])
AC_MSG_NOTICE([  apxs:      $APXS])
AC_MSG_NOTICE([  apachectl: $APACHECTL])
AC_MSG_NOTICE([  libxml2:   $LIBXML2_INC])
AC_MSG_NOTICE([])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
