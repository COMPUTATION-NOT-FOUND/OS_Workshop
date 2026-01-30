//
// Console input and output, to the uart.
// Reads are line at a time.
// Implements special input characters:
//   newline -- end of line
//   control-h -- backspace
//   control-u -- kill line
//   control-d -- end of file
//   control-p -- print process list
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

#define BACKSPACE 0x100
#define C(x)  ((x)-'@')  // Control-x

//
// send one character to the uart.
// called by printf(), and to echo input characters,
// but not from write().
//
void
consputc(int c)
{
  if(c == BACKSPACE){
    // if the user typed backspace, overwrite with a space.
    uartputc_sync('\b'); uartputc_sync(' '); uartputc_sync('\b');
  } else {
    uartputc_sync(c);
  }
}

struct {
  struct spinlock lock;
  
  // input
#define INPUT_BUF_SIZE 128
  char buf[INPUT_BUF_SIZE];
  uint r;  // Read index
  uint w;  // Write index
  uint e;  // Edit index
} cons;

//
// user write()s to the console go here.
//
int
consolewrite(int user_src, uint64 src, int n)
{
  int i;

  for(i = 0; i < n; i++){
    char c;
    if(either_copyin(&c, user_src, src+i, 1) == -1)
      break;
    uartputc(c);
  }

  return i;
}

//
// user read()s from the console go here.
// copy (up to) a whole input line to dst.
// user_dist indicates whether dst is a user
// or kernel address.
//
int
consoleread(int user_dst, uint64 dst, int n)
{
  uint target;
  int c;
  char cbuf;

  target = n;
  acquire(&cons.lock);
  while(n > 0){
    // wait until interrupt handler has put some
    // input into cons.buffer.
    while(cons.r == cons.w){
      if(killed(myproc())){
        release(&cons.lock);
        return -1;
      }
      sleep(&cons.r, &cons.lock);
    }

    c = cons.buf[cons.r++ % INPUT_BUF_SIZE];

    if(c == C('D')){  // end-of-file
      if(n < target){
        // Save ^D for next time, to make sure
        // caller gets a 0-byte result.
        cons.r--;
      }
      break;
    }

    // copy the input byte to the user-space buffer.
    cbuf = c;
    if(either_copyout(user_dst, dst, &cbuf, 1) == -1)
      break;

    dst++;
    --n;

    if(c == '\n'){
      // a whole line has arrived, return to
      // the user-level read().
      break;
    }
  }
  release(&cons.lock);

  return target - n;
}

//
// the console input interrupt handler.
// uartintr() calls this for input character.
// do erase/kill processing, append to cons.buf,
// wake up consoleread() if a whole line has arrived.
//
#define HISTORY_SIZE 10
// History buffer: 10 commands, each up to 128 bytes
static char history[HISTORY_SIZE][INPUT_BUF_SIZE];
static int history_count = 0;
static int history_pos = 0;

void browse_history(int direction) {
    if(history_count == 0) return;

    // 1. Update the current history position
    history_pos += direction;
    
    // Clamp values (don't go past start or end)
    if(history_pos < 0) history_pos = 0;
    if(history_pos >= history_count) history_pos = history_count - 1;

    // 2. Erase the current line on screen
    // We assume the user hasn't pressed Enter yet, so we are editing 'cons.e'
    while(cons.e != cons.w &&
          cons.buf[(cons.e-1) % INPUT_BUF_SIZE] != '\n'){
        cons.e--;
        consputc(BACKSPACE); // Erase visual character
    }

    // 3. Copy the history command to the input buffer
    char *cmd = history[history_pos % HISTORY_SIZE];
    for(int i = 0; cmd[i]; i++){
        cons.buf[cons.e++ % INPUT_BUF_SIZE] = cmd[i];
        consputc(cmd[i]); // Echo back to screen
    }
}

// Track the escape sequence state: 0=Normal, 1=Saw ESC, 2=Saw [
static int esc_seq = 0; 

void
consoleintr(int c)
{
  acquire(&cons.lock);

  // Handle ANSI escape sequences for Arrow Keys
  if(esc_seq) {
    if(esc_seq == 1 && c == '['){ 
        esc_seq = 2; 
        release(&cons.lock); 
        return; 
    }
    if(esc_seq == 2){
      esc_seq = 0;
      if(c == 'A') { browse_history(-1); release(&cons.lock); return; } // Up Arrow
      if(c == 'B') { browse_history(1); release(&cons.lock); return; }  // Down Arrow
    }
    esc_seq = 0; // Reset if invalid sequence
  } else if(c == '\x1b'){ // 0x1B is the ESC key
    esc_seq = 1;
    release(&cons.lock);
    return;
  }

  switch(c){
  case C('P'):  // Print process list.
    procdump();
    break;
  case C('U'):  // Kill line.
    while(cons.e != cons.w &&
          cons.buf[(cons.e-1) % INPUT_BUF_SIZE] != '\n'){
      cons.e--;
      consputc(BACKSPACE);
    }
    break;
  case C('H'): // Backspace
  case '\x7f': // Delete key
    if(cons.e != cons.w){
      cons.e--;
      consputc(BACKSPACE);
    }
    break;
  default:
    if(c != 0 && cons.e-cons.r < INPUT_BUF_SIZE){
      c = (c == '\r') ? '\n' : c;

      // echo back to the user.
      consputc(c);

      // store for consumption by consoleread().
      cons.buf[cons.e++ % INPUT_BUF_SIZE] = c;

      if(c == '\n' || c == C('D') || cons.e-cons.r == INPUT_BUF_SIZE){
        // wake up consoleread() if a whole line (or end-of-file)
        // has arrived.
        cons.w = cons.e;
        wakeup(&cons.r);

        // Save line to history
        int len = cons.e - cons.r;
        if(len > 1){ // Don't save empty lines
            
            // Adjust len to remove the trailing newline if present
            if(cons.buf[(cons.e - 1) % INPUT_BUF_SIZE] == '\n') {
                len--; 
            }

            int idx = history_count % HISTORY_SIZE;
            
            // Copy from console buffer to history buffer
            for(int i = 0; i < len; i++)
              history[idx][i] = cons.buf[(cons.r + i) % INPUT_BUF_SIZE];
            
            history[idx][len] = 0; // Null-terminate string
            history_count++;
        }
        // Reset position to the end for the new prompt
        history_pos = history_count;
      }
    }
    break;
  }
  
  release(&cons.lock);
}

void
consoleinit(void)
{
  initlock(&cons.lock, "cons");

  uartinit();

  // connect read and write system calls
  // to consoleread and consolewrite.
  devsw[CONSOLE].read = consoleread;
  devsw[CONSOLE].write = consolewrite;
}
