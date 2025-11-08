#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

// --- Utility: read command output into malloc'd buffer ---
static char* read_cmd(const char *cmd) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;

    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    if (!buf) { pclose(fp); errno = ENOMEM; return NULL; }

    for (;;) {
        if (cap - len < 4096 + 1) {
            size_t new_cap = cap * 2;
            char *nb = realloc(buf, new_cap);
            if (!nb) { free(buf); pclose(fp); errno = ENOMEM; return NULL; }
            buf = nb; cap = new_cap;
        }
        size_t n = fread(buf + len, 1, cap - len - 1, fp);
        len += n;
        if (n == 0) {
            if (feof(fp)) break;
            int saved = errno; free(buf); pclose(fp); errno = saved ? saved : EIO; return NULL;
        }
    }

    int rc = pclose(fp);
    if (rc == -1) { int saved = errno; free(buf); errno = saved; return NULL; }

    buf[len] = '\0';
    return buf;
}

void runOllamaServer() {
    // Start Ollama server in the background
    if (system("ollama serve > /dev/null 2>&1 &") == -1) {
	fprintf(stderr, "Failed to start Ollama server: %s\n", strerror(errno));
	exit(1);
    }
}

int main(void) {
    // 0) Ensure Ollama server is running
    runOllamaServer();
    // 1) Get the staged diff
    char *diff = read_cmd("git diff --staged --no-color");
    if (!diff) {
        fprintf(stderr, "Error: failed to read staged diff: %s\n", strerror(errno));
        return 1;
    }
    if (diff[0] == '\0') {
        fprintf(stderr, "No staged changes. Stage files first (git add ...).\n");
        free(diff);
        return 1;
    }

    // 2) Create temp file for the prompt
    char tmpl[] = "/tmp/ollama_prompt_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd == -1) {
        fprintf(stderr, "mkstemp failed: %s\n", strerror(errno));
        free(diff);
        return 1;
    }

    FILE *tf = fdopen(fd, "w");
    if (!tf) {
        fprintf(stderr, "fdopen failed: %s\n", strerror(errno));
        close(fd); unlink(tmpl); free(diff);
        return 1;
    }

    // 3) Prompt
    fputs(
        "You are a Git commit message generator.\n"
        "\n"
        "TASK:\n"
        "Write a SINGLE-LINE Git commit subject summarizing the changes below.\n"
        "\n"
        "HARD RULES:\n"
        "- Output ONLY that one line, nothing else.\n"
        "- No code fences, no quotes, no explanations, no prefixes.\n"
        "- Use imperative mood (e.g., 'fix', 'add', 'update', 'remove').\n"
        "- Do NOT echo the diff or any text other than the commit message.\n"
        "- End output immediately after that line.\n"
        "\n"
        "Diff follows:\n\n",
        tf
    );
    fputs(diff, tf);
    fclose(tf);
    free(diff);

    // 4) Run Ollama
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ollama run llama3 < %s", tmpl);

    int rc = system(cmd);

    // 5) Cleanup
    unlink(tmpl);

    if (rc == -1) {
        fprintf(stderr, "system() failed: %s\n", strerror(errno));
        return 1;
    }
    system("pkill -f ollama");
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : 1;
}

