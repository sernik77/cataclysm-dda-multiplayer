# bash completion for cataclysm and cataclysm-tiles

_cataclysm()
{
    local cur prev
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    case "$prev" in
        --userdir|--basepath|--datadir|--savedir|--configdir|--memorialdir)
            COMPREPLY=( $(compgen -d -- "$cur") )
            return ;;
        --optionfile|--keymapfile|--autopickupfile|--motdfile)
            COMPREPLY=( $(compgen -f -- "$cur") )
            return ;;
        --world)
            local saves="${XDG_DATA_HOME:-$HOME/.local/share}/cataclysm-dda/save"
            [ -d "$saves" ] && COMPREPLY=( $(compgen -W "$(ls -1 "$saves" 2>/dev/null)" -- "$cur") )
            return ;;
        --mp-host)
            COMPREPLY=( $(compgen -W "7777" -- "$cur") )
            return ;;
        --mp-pace)
            COMPREPLY=( $(compgen -W "100 250 500" -- "$cur") )
            return ;;
        --mp-grace)
            COMPREPLY=( $(compgen -W "10000 30000 60000" -- "$cur") )
            return ;;
        --seed|--username|--addadmin|--adddebugger|--mp-password)
            return ;;
    esac

    local opts="--help --seed --jsonverify --check-mods --noverify --world --basepath
        --shared --username --addadmin --adddebugger --competitive --worldmenu
        --userdir --datadir --savedir --configdir --memorialdir --optionfile
        --keymapfile --autopickupfile --motdfile --disable-ascii-art
        --mp-host --mp-password --mp-pace --mp-grace --mp-loopback"
    COMPREPLY=( $(compgen -W "$opts" -- "$cur") )
}

complete -F _cataclysm cataclysm cataclysm-tiles
