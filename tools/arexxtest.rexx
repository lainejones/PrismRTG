/* arexxtest.rexx - PrismD's ARexx port PRISM: every command, its answer checked.
   rx DH0:Prism/arexxtest.rexx   (RexxMast must be running) */
OPTIONS RESULTS
TRACE OFF                  /* the refusals below are on purpose */
fails = 0; checks = 0
IF ~SHOW('P', 'PRISM') THEN DO
    SAY 'arexxtest: no PRISM port (is PrismD running?)'
    EXIT 20
END
ADDRESS PRISM

'VERSION';    CALL check 'VERSION', rc = 0 & WORDS(result) = 3, result
'BOARDS';     boards = result; CALL check 'BOARDS', rc = 0 & (boards = 1 | boards = 2), boards
'BOARD';      CALL check 'BOARD', rc = 0 & result ~= '', result
'VRAM';       CALL check 'VRAM', rc = 0 & WORD(result, 1) > 0 & WORD(result, 2) <= WORD(result, 1), result
'MODES';      n = result; CALL check 'MODES', rc = 0 & n > 0, n
'MODE 1';     first = result; CALL check 'MODE 1', rc = 0 & LEFT(first, 3) = '$7A' & WORDS(first) >= 5, first
'MODE' n + 1; CALL check 'MODE past the last fails', rc = 10, rc
'MODEID' WORD(first, 2) WORD(first, 3) WORD(first, 4)
CALL check 'MODEID of MODE 1', rc = 0 & result = WORD(first, 1), result
'MODEID 123 45 8'; CALL check 'MODEID not offered', rc = 5, rc
'POWERSAVE';  old = result; CALL check 'POWERSAVE', rc = 0 & DATATYPE(old, 'W'), old
'POWERSAVE 7'; CALL check 'POWERSAVE 7', rc = 0 & result = 7, result
'POWERSAVE' old
'POWERSAVE 999'; CALL check 'POWERSAVE 999 refused', rc = 10, rc
'DARK';       CALL check 'DARK (awake)', rc = 0 & result = 0, result
'SLEEP';      CALL check 'SLEEP', rc = 0, rc
'DARK';       CALL check 'DARK (asleep)', rc = 0 & result = 1, result
'WAKE';       CALL check 'WAKE', rc = 0, rc
'DARK';       CALL check 'DARK (awake again)', rc = 0 & result = 0, result
'LOG';        oldlog = result; CALL check 'LOG', rc = 0 & (oldlog = 'ON' | oldlog = 'OFF'), oldlog
'LOG ON';     CALL check 'LOG ON', rc = 0 & result = 'ON', result
'LOG' oldlog
'TEST 640 480 8'; CALL check 'TEST 640 480 8', rc = 0 & result = 'OK', result
ADDRESS COMMAND 'Wait 1'
'TESTEND';    CALL check 'TESTEND', rc = 0, rc
'TEST 333 222 8'; CALL check 'TEST of an odd size answers', rc = 0 | rc = 5, rc
'TESTEND'
'HELP';       CALL check 'HELP', rc = 0 & POS('POWERSAVE', result) > 0, result
'NOSUCHTHING'; CALL check 'unknown command', rc = 10, rc
IF boards = 2 THEN DO
    'BOARD 2'; CALL check 'BOARD 2', rc = 0 & result ~= '', result
END
SAY 'arexxtest:' fails 'of' checks 'checks failed'
EXIT fails ~= 0

check: PROCEDURE EXPOSE fails checks
    PARSE ARG what, good, got
    checks = checks + 1
    IF good THEN SAY '  ' LEFT(what, 34) 'ok    ' got
    ELSE DO
        fails = fails + 1
        SAY '  ' LEFT(what, 34) 'FAILED' got
    END
    RETURN
