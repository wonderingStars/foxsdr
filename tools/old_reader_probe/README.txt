old_reader_probe - the proof that an OLD build reads a file the multi-radio build wrote (engine stage 5).

What it does: new_main.cpp is built against THIS tree and writes two settings files with ConfigStore::save (one radio; eight radios plus a plugin's radio). old_main.cpp is
built against the SOURCES OF THE OLD BUILD (git show v0.99.104:src/core/config.hpp and config.cpp, placed in a directory searched first) and does what the old application
does with a settings file: ConfigStore::load, then ConfigStore::save. The old reader must load both files, read radio 1 exactly, and write the file back WITHOUT the `radios`
and `pluginRadio` keys (radios 2 to 8 are forgotten there, radio 1 is not).

To run (Windows, MSVC build tools, a configured build folder with cascade_engine.lib):
  mkdir old && mkdir old\core
  git show v0.99.104:src/core/config.hpp > old\core\config.hpp
  git show v0.99.104:src/core/config.cpp > old\old_config.cpp
  copy tools\old_reader_probe\old_main.cpp old\ & edit R and B in build_probe.bat (the worktree and the build folder), then run it from old\ with new_main.cpp beside.
  new_writer.exe one.json eight.json
  old_reader.exe one.json one_back.json
  old_reader.exe eight.json eight_back.json
The result of the run at the stage 5 commit is in docs/multi-radio-records/engine-stages-5-6.md.
