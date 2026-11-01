# Start ccmatrix as a screensaver when the zsh prompt has been idle.
#
# Add to ~/.zshrc:
#     source /path/to/ccmatrix-idle.zsh
#
# Set CCMATRIX_IDLE (seconds) before sourcing to change the timeout.
# Uses zsh's TMOUT/TRAPALRM, which fire while zsh waits at the prompt, so it
# never interrupts a running command. Bash has no equivalent; use tmux or
# GNU screen instead (see README.md).

TMOUT=${CCMATRIX_IDLE:-300}

TRAPALRM() {
  # Only when ccmatrix is installed and we're on a real terminal.
  if (( $+commands[ccmatrix] )) && [[ -t 1 ]]; then
    ccmatrix -s
    zle && zle reset-prompt
  fi
}
