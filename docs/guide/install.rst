.. Copyright 2003-2025 by Wilson Snyder.
.. SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

.. _Installation:

************
Installation
************

This section discusses how to install Verilator.

.. _Package Manager Quick Install:

Package Manager Quick Install
=============================

Using a distribution's package manager is the easiest way to get
started. (Note distribution packages almost never have the most recent
Verilator version, so we recommend following :ref:`Git Install` below,
instead.) To install as a package:

.. code-block:: shell

   apt-get install verilator   # On Ubuntu

For other distributions, refer to `Repology Verilator Distro Packages
<https://repology.org/project/verilator>`__.

.. _pre-commit Quick Install:

pre-commit Quick Install
=============================

You can use Verilator's `pre-commit <https://pre-commit.com/>`__ hook to
lint your code before committing it.  It encapsulates the :ref:`Verilator
Build Docker Container`, so you need docker on your system to use it.  The
verilator image will be downloaded automatically.

To use the hook, add the following entry to your :code:`.pre-commit-config.yaml`:

.. code-block:: yaml

   repos:
     - repo: https://github.com/verilator/verilator
       rev: v5.026  # or later
       hooks:
         - id: verilator

.. _Git Install:

Git Quick Install
=================

Installing Verilator from Git provides the most flexibility; for additional
options and details, see :ref:`Detailed Build Instructions` below.

In brief, to install from git:

.. code-block:: shell

   # Prerequisites:
   #sudo apt-get install git help2man perl python3 make autoconf g++ flex bison ccache
   #sudo apt-get install libgoogle-perftools-dev numactl perl-doc
   #sudo apt-get install libfl2  # Ubuntu only (ignore if gives error)
   #sudo apt-get install libfl-dev  # Ubuntu only (ignore if gives error)
   #sudo apt-get install zlibc zlib1g zlib1g-dev  # Ubuntu only (ignore if gives error)

   git clone https://github.com/verilator/verilator   # Only first time

   # Every time you need to build:
   unsetenv VERILATOR_ROOT  # For csh; ignore error if on bash
   unset VERILATOR_ROOT  # For bash
   cd verilator
   git pull         # Make sure git repository is up-to-date
   git tag          # See what versions exist
   #git checkout master      # Use development branch (e.g. recent bug fixes)
   #git checkout stable      # Use most recent stable release
   #git checkout v{version}  # Switch to specified release version

   autoconf         # Create ./configure script
   ./configure      # Configure and create Makefile
   make -j `nproc`  # Build Verilator itself (if error, try just 'make')
   sudo make install


.. _Detailed Build Instructions:

Detailed Build Instructions
===========================

This section describes details of the build process and assumes you are
building from Git. For using a pre-built binary for your Linux
distribution, see instead :ref:`Package Manager Quick Install`.


OS Requirements
---------------

Verilator is developed and has primary testing on Ubuntu, with additional
testing on FreeBSD and Apple OS-X. Versions have also been built on Red Hat
Linux, other flavors of GNU/Linux-ish platforms, Windows Subsystem for
Linux (WSL2), Windows under Cygwin, and Windows under MinGW (gcc
-mno-cygwin). Verilated output (not Verilator itself) compiles under all
the options above, plus using MSVC++.


Install Prerequisites
---------------------

To build or run Verilator, you need these standard packages:

.. code-block:: shell

   sudo apt-get install git help2man perl python3 make
   sudo apt-get install g++  # Alternatively, clang
   sudo apt-get install libgz  # Non-Ubuntu (ignore if gives error)
   sudo apt-get install libfl2  # Ubuntu only (ignore if gives error)
   sudo apt-get install libfl-dev  # Ubuntu only (ignore if gives error)
   sudo apt-get install zlibc zlib1g zlib1g-dev  # Ubuntu only (ignore if gives error)

For SystemC:

.. code-block:: shell

   sudo apt-get install libsystemc libsystemc-dev

For constraints:

.. code-block:: shell

   sudo apt-get install z3  # Optional solver

The following is optional but is recommended for nicely rendered command line
help when running Verilator:

.. code-block:: shell

   sudo apt-get install perl-doc

To build or run Verilator, the following are optional but should be installed
for good performance:

.. code-block:: shell

   sudo apt-get install ccache  # If present at build, needed for run
   sudo apt-get install mold  # If present at build, needed for run
   sudo apt-get install libgoogle-perftools-dev numactl

To build Verilator you will need to install these packages; these do not
need to be present to run Verilator:

.. code-block:: shell

   sudo apt-get install git autoconf flex bison

Those developing Verilator itself also need these (see internals.rst):

.. code-block:: shell

   sudo apt-get install clang clang-format-14 cmake gdb gprof graphviz lcov
   sudo apt-get install python3-clang python3-distro yapf3 bear jq
   sudo pip3 install sphinx sphinx_rtd_theme sphinxcontrib-spelling breathe gersemi mbake ruff sarif-tools
   sudo pip3 install git+https://github.com/antmicro/astsee.git
   cpan install Pod::Perldoc


Install SystemC
^^^^^^^^^^^^^^^

SystemC code can be generated from Verilator (with :vlopt:`--sc`) if it is
installed as a package (see above).

Alternatively, from their sources, download `SystemC
<https://www.accellera.org/downloads/standards/systemc>`__.  Follow their
installation instructions. You will need to set the
:option:`SYSTEMC_INCLUDE` environment variable to point to the include
directory with ``systemc.h`` in it, and set the :option:`SYSTEMC_LIBDIR`
environment variable to point to the directory with ``libsystemc.a`` in it.


Install GTKWave
^^^^^^^^^^^^^^^

To make use of Verilator FST tracing you will want `GTKwave
<https://gtkwave.sourceforge.net/>`__ installed, however this is not
required at Verilator build time.

.. code-block:: shell

    sudo apt-get install gtkwave  # Optional Waveform viewer


Install Z3
^^^^^^^^^^

In order to use constrained randomization the `Z3 Theorem Prover
<https://github.com/z3prover/z3>`__ must be installed, however this is
not required at Verilator build time. There are other compatible SMT solvers,
like CVC5/CVC4, but they are not guaranteed to work. Since different solvers are
faster for different scenarios, the solver to use at run-time can be specified
by the environment variable :option:`VERILATOR_SOLVER`.


.. _Obtain Sources:

Obtain Sources
--------------

Get the sources from the git repository: (You need to do this only once,
ever.)

.. code-block:: shell

   git clone https://github.com/verilator/verilator   # Only first time
   ## Note the URL above is not a page you can see with a browser; it's for git only

Enter the checkout and determine what version/branch to use:

.. code-block:: shell

   cd verilator
   git pull        # Make sure we're up-to-date
   git tag         # See what versions exist
   #git checkout master      # Use development branch (e.g. recent bug fix)
   #git checkout stable      # Use most recent release
   #git checkout v{version}  # Switch to specified release version


Auto Configure
--------------

Create the configuration script:

.. code-block:: shell

   autoconf        # Create ./configure script


Eventual Installation Options
-----------------------------

Before configuring the build, you must decide how you're going to
eventually install Verilator onto your system. Verilator will be compiling
the current value of the environment variables :option:`VERILATOR_ROOT`,
:option:`VERILATOR_SOLVER`, :option:`SYSTEMC_INCLUDE`, and
:option:`SYSTEMC_LIBDIR` as defaults into the executable, so they must be
correct before configuring.

These are the installation options:


1. Run-in-Place from VERILATOR_ROOT
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Our personal favorite is to always run Verilator in-place from its Git
directory (don't run ``make install``). This allows the easiest
experimentation and upgrading, and allows many versions of Verilator to
co-exist on a system.

.. code-block:: shell

   export VERILATOR_ROOT=`pwd`   # if your shell is bash
   setenv VERILATOR_ROOT `pwd`   # if your shell is csh
   ./configure
   # Running will use files from $VERILATOR_ROOT, so no install needed

Note after installing (see `Installation`_), a calling program or shell
must set the environment variable :option:`VERILATOR_ROOT` to point to this
Git directory, then execute ``$VERILATOR_ROOT/bin/verilator``, which will
find the path to all needed files.


2. Install into a Specific Prefix
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

You may be an OS package maintainer building a Verilator package, or you
may eventually be installing onto a project/company-wide "CAD" tools disk
that may support multiple versions of every tool. Tell configure the
eventual destination directory name.  We recommend that the destination
location include the Verilator version name:

.. code-block:: shell

   unset VERILATOR_ROOT      # if your shell is bash
   unsetenv VERILATOR_ROOT   # if your shell is csh
   # For the tarball, use the version number instead of git describe
   ./configure --prefix /CAD_DISK/verilator/`git describe | sed "s/verilator_//"`

Note after installing (see `Installation`_), you need to add the path to
the ``bin`` directory to your ``PATH``. Or, if you use `modulecmd
<https://modules.sourceforge.net/>`__, you'll want a module file like the
following:

.. code-block:: shell

   set install_root /CAD_DISK/verilator/{version-number-used-above}
   unsetenv VERILATOR_ROOT
   prepend-path PATH $install_root/bin
   prepend-path MANPATH $install_root/man
   prepend-path PKG_CONFIG_PATH $install_root/share/pkgconfig


3. Install System Globally
^^^^^^^^^^^^^^^^^^^^^^^^^^

The final option is to eventually install Verilator globally, using
configure's default system paths:

.. code-block:: shell

   unset VERILATOR_ROOT      # if your shell is bash
   unsetenv VERILATOR_ROOT   # if your shell is csh
   ./configure

Then after installing (see `Installation`_), the binaries should be in a
location already in your ``$PATH`` environment variable.


Configure
---------

The command to configure the package was described in the previous step.
Developers should configure to have more complete developer tests.
Additional packages may be required for these tests.

.. code-block:: shell

   export VERILATOR_AUTHOR_SITE=1    # Put in your .bashrc
   ./configure --enable-longtests  ...above options...


Compile
-------

Compile Verilator:

.. code-block:: shell

   make -j `nproc`  # Or if error on `nproc`, the number of CPUs in system


Test
----

Check the compilation by running self-tests:

.. code-block:: shell

   make test


Install
-------

If you used any install option other than the `1. Run-in-Place from
VERILATOR_ROOT <#_1_run_in_place_from_verilator_root>`__ scheme, install
the files:

.. code-block:: shell

   make install


.. Docker Build Environment

.. include:: ../../ci/docker/buildenv/README.rst


.. Docker Run Environment

.. include:: ../../ci/docker/run/README.rst
