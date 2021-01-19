'''
Embeddable read-eval-print loop for Python applications.

Example usage:
    # during application startup
    repl = RemoteREPL() # optionally pass in listen host and/or port

    # call this often during app lifetime
    repl.Process()

To connect use telnet or:
    python -m telnetlib 127.0.0.1 9999 # or other host/port as needed

Notes:
- For applications that have restrictions on what can be run from non-main threads,
    be sure to call repl.Process from the appropriate thread (e.g. a game's main thread)
- TODO: support _ for last-result shortcut
'''

import code, time, threading, sys, socketserver, selectors, traceback, queue

# patchable - TODO use python logging
log = print
def logTB():
    for line in traceback.format_exc().split('\n'):
        log(line)

class RemoteREPL(socketserver.TCPServer):
    def __init__(self, host='', port=9999, env=None): # env is e.g. globals()
        super().__init__((host, port), REPLRequestHandler)
        sys.displayhook = self.SysDisplayHook
        self.incoming = queue.Queue()
        self.outgoing = queue.Queue()
        self.lastProcess = 0

        class EmbeddedConsole(code.InteractiveConsole):
            def write(self, value):
                '''called on errors/tracebacks'''
                self.server.outgoing.put(value + '\n')

        if env is None:
            env = globals()
        self.console = EmbeddedConsole(env)
        self.console.server = self

        t = threading.Thread(target=self.serve_forever)
        t.daemon = True
        t.start()

    def SysDisplayHook(self, value):
        '''called on interactive writes to sys.stdout'''
        if value is not None:
            self.outgoing.put(repr(value)+'\n')

    def Process(self):
        '''Should be called periodically from the application's main thread to execute the next waiting command, if any'''
        now = time.time()
        since = now - self.lastProcess
        self.lastProcess = now
        try:
            try:
                next = self.incoming.get_nowait()
                more = self.console.push(next)

                # echo back a different prompt based on whether or not we detected the command as complete
                if more:
                    self.outgoing.put('... ')
                else:
                    self.outgoing.put('>>> ')
            except queue.Empty:
                pass
        except:
            logTB()

class REPLRequestHandler(socketserver.BaseRequestHandler):
    def OnReadable(self, sock):
        '''Reads any incoming input from the remote side and enqueues it for a call to Process'''
        # TODO: use recv_into, a buffer, etc. instead of += strings
        more = sock.recv(4096)
        if not more:
            self.keepRunning = False
        else:
            self.readBuffer += more.decode('utf8')
            if '\n' in self.readBuffer:
                line, self.readBuffer = self.readBuffer.split('\n', 1)
                if line.strip() == 'exit()':
                    # Special case: instead of shutting down the whole process, just disconnect
                    self.keepRunning = False
                else:
                    self.server.incoming.put(line)

    def handle(self):
        '''Reads/writes data until the connection closes'''
        self.readBuffer = ''
        self.keepRunning = True
        selector = selectors.DefaultSelector()
        selector.register(self.request, selectors.EVENT_READ, self.OnReadable)
        log('REPL starting')
        self.server.outgoing.put('Connected to remote REPL\n')
        self.server.outgoing.put('>>> ')
        try:
            while self.keepRunning:
                for key, mask in selector.select(0.05):
                    key.data(key.fileobj)

                # Send queued output
                while 1:
                    try:
                        out = self.server.outgoing.get_nowait()
                        self.request.sendall(out.encode('utf8'))
                    except queue.Empty:
                        break
        finally:
            log('REPL quitting')

if __name__ == '__main__':
    repl = RemoteREPL()
    while 1:
        repl.Process()
        time.sleep(0.1)


