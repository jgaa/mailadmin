# Project

This is a C++ project that provide an API to manage a running courier mail server.

It's designed to run as a conteiner with file-access to the servers configuration and spool files (to create or remove user's spool directories), 
running as the same user as the mail server.

# Rules and guidelines

Cache the files locally (not checked into git) and either refresh or check the timestamp on the HTTP server when you refresh your Agents instructions, or checkout the agents repository https://github.com/jgaa/agents.git outside the project and keep it up to date before you re-read the files.

https://raw.githubusercontent.com/jgaa/agents/refs/heads/main/rules/tlv-agents.md
https://raw.githubusercontent.com/jgaa/agents/refs/heads/main/rules/tlv-agents-cpp.md

