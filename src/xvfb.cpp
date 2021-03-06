#include "xvfb.hpp"

#include "temp_dir.hpp"

#include <csignal>

#include <unistd.h>
#include <sys/wait.h>

namespace {

optional<int> parseDisplay(string displayStr) {
    optional<int> empty;
    if(displayStr.empty() || displayStr[displayStr.size() - 1] != '\n') {
        return empty;
    }
    optional<int> display = parseString<int>(
        displayStr.substr(0, displayStr.size() - 1)
    );
    if(display && *display >= 0) {
        return display;
    } else {
        return empty;
    }
}

string generateCookie() {
    string cookie;
    random_device dev;
    uniform_int_distribution<int> dist(0, 15);
    for(int i = 0; i < 32; ++i) {
        int val = dist(dev);
        if(val < 10) {
            cookie.push_back('0' + val);
        } else {
            cookie.push_back('a' + (val - 10));
        }
    }
    return cookie;
}

void addCookieToXAuthFile(string path, int display, string cookie) {
    string xauthCmd = "xauth -f " + path + " source -";
    FILE* proc = popen(xauthCmd.c_str(), "w");
    if(proc == nullptr) {
        LOG(ERROR) << "Running xauth failed";
        CHECK(false);
    }

    string input = "add :" + toString(display) + " . " + cookie + "\n";

    CHECK(fwrite(input.data(), 1, input.size(), proc) == input.size());
    if(pclose(proc) != 0) {
        LOG(ERROR) << "Running xauth failed (nonzero exit status)";
        CHECK(false);
    }
}

}

Xvfb::Xvfb(CKey) {
    LOG(INFO) << "Starting Xvfb X server as child process";

    tempDir_ = TempDir::create();
    xAuthPath_ = tempDir_->path() + "/.Xauthority";

    // Create empty .Xauthority file
    ofstream fp;
    fp.open(xAuthPath_.c_str(), fp.out | fp.binary);
    fp.close();
    CHECK(fp.good());

    // Add dummy cookie to stop server from accepting all connections
    addCookieToXAuthFile(xAuthPath_, 0, generateCookie());

    // Pipe through which Xvfb sends us the display number
    int displayFds[2];
    CHECK(!pipe(displayFds));
    int readDisplayFd = displayFds[0];
    int writeDisplayFd = displayFds[1];

    pid_ = fork();
    CHECK(pid_ != -1);
    if(!pid_) {
        // Xvfb subprocess:
        CHECK(!close(readDisplayFd));

        // Move the X server process to its own process group, as otherwise
        // Ctrl+C sent to the parent would stop the X server before we have
        // time to shut the parent down
        CHECK(!setpgid(0, 0));

        string writeDisplayFdStr = toString(writeDisplayFd);
        execlp(
            "Xvfb",
            "Xvfb",
            "-displayfd", writeDisplayFdStr.c_str(),
            "-auth", xAuthPath_.c_str(),
            "-screen", "0", "640x480x24",
            (char*)nullptr
        );

        // If exec succeeded, this should not be reachable
        CHECK(false);
    }

    // Parent process:
    CHECK(!close(writeDisplayFd));

    string displayStr;
    const size_t BufSize = 64;
    char buf[BufSize];

    while(true) {
        ssize_t readCount = read(readDisplayFd, buf, BufSize);
        if(readCount == 0) {
            break;
        }
        if(readCount < 0) {
            CHECK(errno == EINTR);
            readCount = 0;
        }
        displayStr.append(buf, readCount);
    }
    CHECK(!close(readDisplayFd));

    optional<int> display = parseDisplay(displayStr);
    if(!display) {
        LOG(ERROR) << "Starting Xvfb failed";
        CHECK(false);
    }

    display_ = *display;
    LOG(INFO) << "Xvfb X server :" << display_ << " successfully started";

    // Now that we know the display number, we can add the xauth rule we
    // actually use
    addCookieToXAuthFile(xAuthPath_, display_, generateCookie());

    running_ = true;
}

Xvfb::~Xvfb() {
    if(running_) {
        shutdown();
    }
}

void Xvfb::setupEnv() {
    string displayStr = ":" + toString(display_);
    CHECK(!setenv("DISPLAY", displayStr.c_str(), true));
    CHECK(!setenv("XAUTHORITY", xAuthPath_.c_str(), true));
}

void Xvfb::shutdown() {
    if(!running_) {
        return;
    }

    LOG(INFO) << "Sending SIGTERM to the Xvfb X server child process to shut it down";
    if(kill(pid_, SIGTERM) != 0) {
        LOG(WARNING) << "Could not send SIGTERM signal to Xvfb, maybe it has already shut down?";
    }

    LOG(INFO) << "Waiting for Xvfb child process to shut down";
    CHECK(waitpid(pid_, nullptr, 0) == pid_);

    LOG(INFO) << "Successfully shut down Xvfb X server";

    if(unlink(xAuthPath_.c_str())) {
        LOG(WARNING) << "Unlinking file " << xAuthPath_ << " failed";
    }

    running_ = false;
}
