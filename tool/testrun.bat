@rem $Id$
@rem Copyright (c) 2013-2020 Ravenbrook Limited. See end of file for license.
@rem
@rem This program runs a series of test cases, capturing the output of
@rem each one to a temporary file. In addition, the output of any test
@rem case that fails gets printed to standard output so that it is not
@rem lost when the test is running on a temporary build server (see
@rem job003489). Finally, it prints a summary of passes and failures, and
@rem if there were any failures, it exits with a non-zero status code.
@rem
@rem Usage:
@rem
@rem     testrun.bat PLATFORM VARIETY ( SUITE | CASE1 CASE2 ... )

@echo off

@rem Find test case database in same directory as this script.
@rem The incantation %%~dpF% expands %%F to a drive letter and path only.
@rem See "help for" for more details.
for %%F in ("%0") do set TEST_CASE_DB=%%~dpF%testcases.txt

set PFM=%1
shift
set VARIETY=%1
shift
set TESTSUITE=%1

@rem Make a temporary output directory for the test logs.
set LOGDIR=%TMP%\mps-%PFM%-%VARIETY%-log
echo MPS test suite
echo Logging test output to %LOGDIR%
echo Test directory: %PFM%\%VARIETY%
echo Test case database: %TEST_CASE_DB%
if exist %LOGDIR% rmdir /q /s %LOGDIR%
mkdir %LOGDIR%

@rem Determine which tests to run.
set EXCLUDE=
if "%TESTSUITE%"=="testrun"      set EXCLUDE=LNX
if "%TESTSUITE%"=="testci"       set EXCLUDE=BNX
if "%TESTSUITE%"=="testall"      set EXCLUDE=NX
if "%TESTSUITE%"=="testansi"     set EXCLUDE=LNTX
if "%TESTSUITE%"=="testpollnone" set EXCLUDE=LNPTX

@rem Ensure that test cases don't pop up dialog box on abort()
set MPS_TESTLIB_NOABORT=true
set TEST_COUNT=0
set PASS_COUNT=0
set FAIL_COUNT=0
set SEPARATOR=----------------------------------------

if "%EXCLUDE%"=="" goto :args
for /f "tokens=1" %%T IN ('type "%TEST_CASE_DB%" ^|^
     findstr /b /r [abcdefghijklmnopqrstuvwxyz] ^|^
     findstr /v /r =[%EXCLUDE%]') do call :run_test %%T
goto :done

:args
if "%1"=="" goto :done
call :run_test %1
shift
goto :args

:done
if "%FAIL_COUNT%"=="0" (
    echo Tests: %TEST_COUNT%. All tests pass.
    exit /b 0
) else (
    echo Tests: %TEST_COUNT%. Passes: %PASS_COUNT%. Failures: %FAIL_COUNT%.
    exit /b 1
)

:run_test
set /a TEST_COUNT=%TEST_COUNT%+1
set LOGTEST=%LOGDIR%\%TEST_COUNT%-%1
echo Running %1
%PFM%\%VARIETY%\%1 > %LOGTEST%
if "%errorlevel%"=="0" (
    set /a PASS_COUNT=%PASS_COUNT%+1
) else (
    echo %SEPARATOR%%SEPARATOR%
    type %LOGTEST%
    echo %SEPARATOR%%SEPARATOR%
    set /a FAIL_COUNT=%FAIL_COUNT%+1
)
exit /b


@rem C. COPYRIGHT AND LICENSE
@rem
@rem Copyright (C) 2013-2020 Ravenbrook Limited <https://www.ravenbrook.com/>.
@rem
@rem Redistribution and use in source and binary forms, with or without
@rem modification, are permitted provided that the following conditions are
@rem met:
@rem
@rem 1. Redistributions of source code must retain the above copyright
@rem    notice, this list of conditions and the following disclaimer.
@rem
@rem 2. Redistributions in binary form must reproduce the above copyright
@rem   notice, this list of conditions and the following disclaimer in the
@rem   documentation and/or other materials provided with the
@rem   distribution.
@rem
@rem THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
@rem IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
@rem TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
@rem PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
@rem HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
@rem SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
@rem LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
@rem DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
@rem THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
@rem (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
@rem OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

