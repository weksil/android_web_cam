@echo off
setlocal
set APP_HOME=%~dp0
if not defined JAVA_HOME set JAVA_HOME=C:\Program Files\Android\Android Studio\jbr
if not exist "%JAVA_HOME%\bin\java.exe" (
  echo JAVA_HOME is not a valid JDK: "%JAVA_HOME%"
  exit /b 1
)
"%JAVA_HOME%\bin\java.exe" -Xmx64m -classpath "%APP_HOME%gradle\wrapper\gradle-wrapper.jar" org.gradle.wrapper.GradleWrapperMain %*
