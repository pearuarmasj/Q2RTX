@echo off

IF "%BUILD_DIRECTORY%" == "" SET BUILD_DIRECTORY=build

IF NOT EXIST %BUILD_DIRECTORY% (
	echo build directory not found
	exit /b 1
)

cd %BUILD_DIRECTORY%

REM Only pass MSBuild flags when using a Visual Studio solution generator.
REM The build directory may contain '*.slnx' (VS 2022+), which must NOT trigger this.
set "EXTRA_FLAGS="

IF NOT EXIST build.ninja (
	for %%F in (*.sln) do (
		if /I "%%~xF"==".sln" set "EXTRA_FLAGS=-- /nologo /verbosity:minimal"
	)
)

cmake --build . --target shaders %EXTRA_FLAGS%
cd ..

