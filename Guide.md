# Fgithub CLI User Guide

**Fgithub CLI v1.1.0**

Fgithub CLI is a Windows command-line interface for GitHub, implemented in C using Windows Native APIs and the GitHub API.

## 1. Getting Started

### Running Fgithub CLI

Run `fgh.exe` to start the interactive Fgithub CLI terminal.

```text
Fgithub CLI 1.1.0
Type help for commands.
fgh>
```

Enter a command and press Enter.

To exit:

```text
exit
```

or:

```text
quit
```

### Help

```text
help
```

Check the version:

```text
version
```

You can also use these directly from Windows Command Prompt or PowerShell:

```text
fgh.exe --help
fgh.exe --version
```

`-h` and `-v` are also supported.

---

# 2. Basic Syntax

Fgithub CLI uses a command-oriented syntax with names and parenthesized arguments.

String argument:

```text
Name("example")
```

Numeric argument:

```text
(123)
```

File argument:

```text
Content([C:\path\file.txt])
```

Example:

```text
login name("myusername") password("github_token")
```

Strings can contain spaces when enclosed in double quotes:

```text
Title("My first issue")
```

---

# 3. GitHub Authentication

## Login

```text
login name("USERNAME") password("TOKEN")
```

Example:

```text
login name("myuser") password("github_token")
```

The value supplied to `password(...)` is a **GitHub access token, not your GitHub account password**.

Check the current login status:

```text
login status
```

Log out:

```text
logout
```

Fgithub CLI uses Windows DPAPI to protect the locally stored authentication token.

---

# 4. Repositories

## Create a Repository

```text
New repository Name("REPOSITORY") Accessibility("Public")
```

For a private repository:

```text
New repository Name("REPOSITORY") Accessibility("private")
```

Both `Public` and `private` are supported.

## Download a Repository

```text
repository download("OWNER/REPO")
```

Example:

```text
repository download("7109jun/Fgithub-CLI")
```

For repositories belonging to the authenticated user, the repository name can also be used:

```text
repository download("Fgithub-CLI")
```

## Fork a Repository

```text
fork repository("REPO") original author("USER")
```

Example:

```text
fork repository("Fgithub-CLI") original author("7109jun")
```

---

# 5. Committing Files

Fgithub CLI can commit files directly to a GitHub repository.

## Commit Text

```text
commit Repository("OWNER/REPO") Branch("main") Content("Hello World") Name("hello.txt")
```

Example:

```text
commit Repository("7109jun/Fgithub-CLI") Branch("main") Content("Hello Fgithub CLI!") Name("hello.txt")
```

## Commit a Local File

Use `[FILE_PATH]` with the `Content` argument:

```text
commit Repository("OWNER/REPO") Branch("main") Content([C:\test\hello.txt]) Name("hello.txt")
```

The difference is:

```text
Content("TEXT")
```

uses directly supplied text, while:

```text
Content([PATH])
```

reads the contents of a local file.

---

# 6. Branches

## Create a Branch

```text
branch create("BRANCH")
```

Example:

```text
branch create("development")
```

## List Branches

```text
branch list
```

---

# 7. Issues

## List Issues

```text
github issue list
```

## Create an Issue

```text
github issue create Title("TITLE") body("BODY")
```

Example:

```text
github issue create Title("Bug report") body("The application crashes when opening this file.")
```

## View an Issue

```text
github issue view (1)
```

or:

```text
github issue view 1
```

---

# 8. Pull Requests

## Create a Pull Request

```text
github pr create Title("TITLE") branch("HEAD") base("BASE")
```

Example:

```text
github pr create Title("Add new feature") branch("development") base("main")
```

## List Pull Requests

```text
github pr list
```

## Merge a Pull Request

```text
github pr merge (1)
```

or:

```text
github pr merge 1
```

---

# 9. Releases

Create a release:

```text
release create Tag("TAG") Title("TITLE") Content("PATH")
```

Example:

```text
release create Tag("v1.1.0") Title("Fgithub CLI v1.1.0") Content("release.txt")
```

`Content` specifies the path to the file used for the release content.

---

# 10. GitHub Search

## Search Repositories

```text
github search repository("QUERY")
```

Example:

```text
github search repository("operating system")
```

## Search Users

```text
github search user("QUERY")
```

Example:

```text
github search user("7109jun")
```

## Search Issues

```text
github search issue("QUERY")
```

Example:

```text
github search issue("bug")
```

## Search Pull Requests

```text
github search pr("QUERY")
```

Example:

```text
github search pr("feature")
```

Search results are limited to 20 results.

---

# 11. User Information

## View User Information

```text
github user("LOGIN")
```

Example:

```text
github user("7109jun")
```

## List a User's Repositories

```text
github user repositories("LOGIN")
```

Example:

```text
github user repositories("7109jun")
```

## List Followers

```text
github user followers("LOGIN")
```

Example:

```text
github user followers("7109jun")
```

## List Following

```text
github user following("LOGIN")
```

Example:

```text
github user following("7109jun")
```

---

# 12. Configuration

Fgithub CLI provides configuration commands for commonly used settings.

## Default Branch

Set the default branch:

```text
config set default-branch("main")
```

Check the current value:

```text
config get default-branch
```

## Editor

Set the editor:

```text
config set editor("code")
```

Example:

```text
config set editor("notepad")
```

Check the current editor:

```text
config get editor
```

## Default Repository

Set the default repository:

```text
config set repository("OWNER/REPO")
```

Check the current repository:

```text
config get repository
```

---

# 13. Command Reference

```text
Authentication

login name("USER") password("TOKEN")
login status
logout


Repository

New repository Name("NAME") Accessibility("Public|private")
repository download("OWNER/REPO")
fork repository("REPO") original author("USER")


Code

commit Repository("OWNER/REPO") Branch("BRANCH") Content("TEXT") Name("FILE")
commit Repository("OWNER/REPO") Branch("BRANCH") Content([PATH]) Name("FILE")

branch create("NAME")
branch list


Issues

github issue list
github issue create Title("TITLE") body("BODY")
github issue view (NUMBER)


Pull Requests

github pr create Title("TITLE") branch("HEAD") base("BASE")
github pr list
github pr merge (NUMBER)


Release

release create Tag("TAG") Title("TITLE") Content("PATH")


Search

github search repository("QUERY")
github search user("QUERY")
github search issue("QUERY")
github search pr("QUERY")


User

github user("LOGIN")
github user repositories("LOGIN")
github user followers("LOGIN")
github user following("LOGIN")


Configuration

config set default-branch("main")
config set editor("code")
config set repository("OWNER/REPO")

config get default-branch
config get editor
config get repository


Other

help
version
exit
quit
```

---

# 14. Common Examples

## Login and Create a Repository

```text
login name("myuser") password("TOKEN")
New repository Name("my-project") Accessibility("Public")
```

## Commit a File

```text
commit Repository("myuser/my-project") Branch("main") Content([C:\project\README.md]) Name("README.md")
```

## Create a Branch

```text
branch create("development")
```

## Create an Issue

```text
github issue create Title("Fix login bug") body("Login currently fails.")
```

## Create a Pull Request

```text
github pr create Title("Fix login") branch("development") base("main")
```

## Search for a Repository

```text
github search repository("Fgithub CLI")
```

## View a User

```text
github user("7109jun")
```

## Exit

```text
exit
```

---

# 15. Troubleshooting

### `Not logged in.`

Log in first:

```text
login name("USER") password("TOKEN")
```

### `unknown command; type help`

The command is not recognized.

Run:

```text
help
```

to view the supported commands.

### `invalid syntax`

The command syntax or argument format is incorrect.

For example:

```text
login name("USER") password("TOKEN")
```

Make sure the required parentheses and arguments are present.

### Unclosed Quotes

Strings must be enclosed with matching double quotes.

Incorrect:

```text
Title("Hello
```

Correct:

```text
Title("Hello")
```

### Local Files

Use square brackets when specifying a local file:

```text
Content([C:\project\file.txt])
```

---

# 16. Running from the Command Line

Fgithub CLI can also execute commands directly from the Windows command line instead of starting the interactive shell.

Examples:

```text
fgh.exe --version
```

```text
fgh.exe --help
```

You can also use:

```text
fgh.exe version
```

and other supported Fgithub CLI commands.

---

# 17. Supported Environment

Fgithub CLI is currently designed for **Windows** and uses Windows Native APIs.

Main Windows APIs/components used include:

* **WinHTTP** — HTTP/HTTPS communication
* **DPAPI** — Local token protection
* **Shell32** — Optional editor integration

Current version:

```text
Fgithub CLI 1.1.0
```

---

# 18. Quick Start

For first-time users:

```text
1. Run fgh.exe

2. login name("YOUR_USERNAME") password("YOUR_TOKEN")

3. login status

4. New repository Name("my-project") Accessibility("Public")

5. branch list

6. commit Repository("YOUR_USERNAME/my-project") Branch("main") Content("Hello!") Name("hello.txt")

7. github issue list

8. exit
```

You can always view the available commands with:

```text
fgh> help
```

---

**Fgithub CLI v1.1.0**

GitHub from the command line.
