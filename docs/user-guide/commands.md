# Commands and aliases

The full command table (every command, its aliases and a one-line description) is generated from
the program itself by the release build:

    tbcad --list-commands commands.md

Releases ship that generated file in place of this note. To see the live list at any time, run
`tbcad --list-commands -` to print it to the console,.

Aliases are short names for commands (for example `L` for `LINE`). Add your own in
`aliases.json`, see [Configuration](configuration.md).
