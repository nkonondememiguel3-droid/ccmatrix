# ccmatrix

A `cmatrix` clone in C: falling green characters, katakana, rainbow trails,
characters that flicker as they fall, a message mode where the rain briefly
resolves into lines of text, a config file, and screensaver hooks.

## Build and install

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/ccmatrix -c

sudo cmake --install build          # binary to /usr/local/bin
```

Installing also puts the example config, the sample message file and the
screensaver hooks under `/usr/local/share/`.

## Options

| Option       | Meaning                                                        |
|--------------|----------------------------------------------------------------|
| `-s`         | Screensaver mode: any key or mouse movement exits              |
| `-u delay`   | Update delay, 0 (fastest) to 10 (slowest); default 4           |
| `-C colour`  | Trail colour: green red blue white yellow cyan magenta black   |
| `-r`         | Rainbow trails (overrides `-C`)                                |
| `-c`         | Half-width katakana instead of letters and digits              |
| `-p file`    | Message mode (see below)                                       |
| `-F percent` | How often trail characters flicker, 0–100; default 6           |
| `-N`         | Ignore the config file                                         |
| `-h`         | Help                                                           |

While it runs: `q` or Ctrl-C quits, and `0`–`9` change the speed.

## Message mode

```sh
ccmatrix -c -p messages/psalm51-ja.txt
```

Each non-empty line of the file is shown in turn, looping back to the start
at the end. For each one:

1. Normal rain falls for `message_interval` seconds.
2. The line is laid out in the middle of the screen. As drops fall through
   it, each letter they touch locks in place and stays bright.
3. Once every letter is showing, the text holds for `message_hold` seconds.
4. The letters drop back into the rain one by one.

The file must be UTF-8. Japanese, Chinese and Korean characters take two
columns each, and lines too wide for the window wrap. Text with spaces wraps
at word boundaries, and text without spaces (like Japanese) wraps between any
two characters. If you resize the window while a message is showing, it is
laid out again for the new size.

## Config file

ccmatrix reads `key = value` settings from the first of these that is set:

1. `$CCMATRIX_CONFIG`
2. `$XDG_CONFIG_HOME/ccmatrix/config`
3. `~/.config/ccmatrix/config`

```sh
mkdir -p ~/.config/ccmatrix
cp ccmatrix.conf.example ~/.config/ccmatrix/config
```

See `ccmatrix.conf.example` for every setting. Command-line options override
the file. A mistake in the file is reported with its line number, and
ccmatrix exits without starting. Use `-N` to run with the defaults instead.

## Using it as a screensaver

`ccmatrix -s` behaves like a screensaver:

- Any key, click or mouse movement exits.
- Input in the first half-second is ignored, so the key press that started
  it doesn't close it again.
- Your screen and terminal settings are restored on exit, including when the
  window is closed or the program is killed.

To start it automatically when you're idle, use one of these hooks:

**tmux** (any shell). Add `contrib/tmux.conf` to `~/.tmux.conf`. It starts
after 5 idle minutes, and prefix + `X` starts it straight away.

**GNU screen.** Add `contrib/screenrc` to `~/.screenrc`.

**zsh.** Add `source /path/to/contrib/ccmatrix-idle.zsh` to `~/.zshrc`. It
starts after 5 idle minutes at the prompt; set `CCMATRIX_IDLE=600` before the
`source` line to change that. It never interrupts a running command.

**X11 desktop.** Run it full-screen in a terminal from an idle locker, e.g.

```sh
xautolock -time 5 -locker "xterm -fullscreen -bg black -e ccmatrix -s -c" &
```

ccmatrix does not lock your session. Any key brings back the terminal
without asking for a password. For a locked screen, run a real locker (e.g.
`vlock`, `i3lock`) after it.

## Files

- `main.c`: terminal handling, animation, message mode
- `config.c`/`config.h`: options, config file parsing
- `text.c`/`text.h`: UTF-8 decoding and encoding, character widths, message
  file loading
