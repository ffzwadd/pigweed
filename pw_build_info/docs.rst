.. _module-pw_build_info:

-------------
pw_build_info
-------------

.. warning::
  This module is under construction and may not be ready for use.

pw_build_info provides tooling, build integration, and libraries for generating,
embedding, and parsing build-related information that is embedded into
binaries. Simple numeric version numbering doesn't typically express things
like where the binary originated, what devices it's compatible with, whether
local changes were present when the binary was built, and more. pw_build_info
simplifies the process of integrating rich version metadata to answer more
complex questions about compiled binaries.

GNU Build IDs
=============
This module provides C++ and python libraries for reading GNU build IDs
generated by the link step of a C++ executable. These build IDs are essentially
hashes of the final linked binary, meaning two identical binaries will have
identical build IDs. This can be used to accurately identify matching
binaries.

Linux executables that depend on the ``build_id`` GN target will automatically
generate GNU build IDs. Windows and macOS binaries cannot use this target as
the implementation of GNU build IDs depends on the ELF file format.

Embedded targets must first explicitly place the GNU build ID section into a
non-info section of their linker script that is readable by the firmware. The
following linker snippet may be copied into a read-only section (just like the
.rodata or .text sections):

.. literalinclude:: build_id_linker_snippet.ld

This snippet may be placed directly into an existing section, as it is not
required to live in its own dedicated section. When opting to create a
dedicated section for the build ID to reside in, Pigweed recommends naming the
section ``.note.gnu.build-id`` as it makes it slightly easier for tools to
parse the build ID out of a binary. After the linker script has been properly
set up, the ``build_id`` GN target may be used to read the build ID at
runtime.

Python tooling
--------------
GNU build IDs can be parsed out of ELF files using the ``build_id`` python tool.
Simply point the tool to a binary with a GNU build ID and the build ID will be
printed out if it is found.

.. code-block:: sh

  $ python -m pw_build_info.build_id my_device_image.elf
  d43cce74f18522052f77a1fa3fb7a25fe33f40dd
