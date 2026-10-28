_noctalia_files() {
  local cur="${COMP_WORDS[COMP_CWORD]}"
  if [[ $COMP_CWORD -eq 2 && ${COMP_WORDS[1]} == completions ]]; then
    COMPREPLY=( $(compgen -W 'bash zsh fish' -- "$cur") )
  elif [[ $COMP_CWORD -eq 1 ]]; then
    COMPREPLY=( $(compgen -W '--help --version completions' -- "$cur") )
  else
    COMPREPLY=()
  fi
}
complete -F _noctalia_files noctalia-files
