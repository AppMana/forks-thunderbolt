savedcmd_thunderbolt_net.mod := printf '%s\n'   main.o trace.o | awk '!x[$$0]++ { print("./"$$0) }' > thunderbolt_net.mod
