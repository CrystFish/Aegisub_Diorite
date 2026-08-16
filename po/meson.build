i18n = import('i18n')
py = import('python').find_installation()
# This is currently busted on OSX
# and incomplete on any platform.
# It misses translatable strings not directly found in either
# C++ source, desktop or appdata file. This affects strings
# of the Windows installer (iss), from Lua scripts and JSON files.
# Until a solution is found, POT updates should continue to use make_pot.sh.
i18n.gettext('aegisub',
	         args: [
	            '-k_', '-kSTR_MENU', '-kSTR_DISP', '-kSTR_HELP', '-kwxT',
	            '-kfmt_tl', '-kfmt_plural:2,3', '-kwxPLURAL:1,2',
	            '--sort-by-file'
	         ],
	         install_dir: localedir)

# Local Windows builds only: compile the catalogs into build/locale next to
# the executable so running aegisub.exe straight from the build tree is
# localized. Uses the bundled pure-Python compiler (tools/genmo.py), so no
# gettext installation is needed locally. GitHub Actions packages
# translations via the installer/portable targets and must not be affected.
if host_machine.system() == 'windows'
	ci = run_command(py, '-c',
	    'import os; print("ci" if os.environ.get("GITHUB_ACTIONS", "").lower() == "true" or os.environ.get("CI", "").lower() == "true" else "local")',
	    check: false)
	if ci.stdout().strip() != 'ci'
		langs = run_command(py, '-c',
		    'import pathlib, sys; print("\\n".join(x for x in pathlib.Path(sys.argv[1]).read_text().split() if x and not x.startswith("#")))',
		    meson.current_source_dir() / 'LINGUAS',
		    check: true).stdout().split()
		foreach lang : langs
			custom_target('locale-' + lang,
				build_by_default: true,
				input: lang + '.po',
				output: 'locale-' + lang + '.stamp',
				command: [py, meson.global_source_root() / 'tools' / 'genmo.py', '@INPUT@',
				          meson.global_build_root() / 'locale' / lang / 'LC_MESSAGES' / 'aegisub.mo'])
		endforeach
	endif
endif
