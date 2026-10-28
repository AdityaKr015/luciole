complete -c noctalia-files -f
complete -c noctalia-files -n '__fish_use_subcommand' -l help -d 'Show help'
complete -c noctalia-files -n '__fish_use_subcommand' -l version -d 'Show version'
complete -c noctalia-files -n '__fish_use_subcommand' -a completions -d 'Print shell completions'
complete -c noctalia-files -n '__fish_seen_subcommand_from completions' -a 'bash zsh fish'
