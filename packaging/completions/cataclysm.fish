# fish completion for cataclysm and cataclysm-tiles
# Installed once per command name (cataclysm.fish, cataclysm-tiles.fish).

for cmd in cataclysm cataclysm-tiles
    complete -c $cmd -f
    complete -c $cmd -l help -d 'Show help and exit'
    complete -c $cmd -l seed -x -d 'Random number generator seed'
    complete -c $cmd -l jsonverify -d 'Check all JSON data and exit'
    complete -c $cmd -l check-mods -x -d 'Check the given mods and exit'
    complete -c $cmd -l noverify -d 'Skip JSON verification'
    complete -c $cmd -l world -x -d 'Load the first save in this world' \
        -a '(path basename (path filter -d (set -q XDG_DATA_HOME; and echo $XDG_DATA_HOME; or echo ~/.local/share)/cataclysm-dda/save/*))'
    complete -c $cmd -l basepath -x -a '(__fish_complete_directories)' -d 'Base path for game data'
    complete -c $cmd -l shared -d 'Map-sharing mode'
    complete -c $cmd -l username -x -d 'Map-sharing user name'
    complete -c $cmd -l addadmin -x -d 'Map-sharing admin'
    complete -c $cmd -l adddebugger -x -d 'Map-sharing debugger'
    complete -c $cmd -l competitive -d 'Map-sharing competitive mode'
    complete -c $cmd -l worldmenu -d 'Map-sharing world menu'
    for d in userdir datadir savedir configdir memorialdir
        complete -c $cmd -l $d -x -a '(__fish_complete_directories)' -d "Path for $d"
    end
    for f in optionfile keymapfile autopickupfile motdfile
        complete -c $cmd -l $f -r -F -d "Path for $f"
    end
    complete -c $cmd -l disable-ascii-art -d 'No ASCII art in menus'
    complete -c $cmd -l mp-host -x -a 7777 -d 'Host a co-op game on this TCP port'
    complete -c $cmd -l mp-password -x -d 'Password for joining players'
    complete -c $cmd -l mp-pace -x -a '100 250 500' -d 'Explore: longest wait per turn (ms)'
    complete -c $cmd -l mp-grace -x -a '10000 30000 60000' -d 'Tactical: wait before auto-pause (ms)'
    complete -c $cmd -l mp-loopback -d 'Accept co-op players from this computer only'
end
