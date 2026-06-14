import subprocess
import sys

result = subprocess.run(['powershell', '-Command',
    'Get-Process | Where-Object {$_.MainWindowHandle -ne 0} | Select-Object Id,ProcessName'], 
    capture_output=True, text=True)
print("=== GUI Processes ===")
print(result.stdout)
print(result.stderr)

result2 = subprocess.run(['powershell', '-Command',
    'tasklist /FI "IMAGENAME eq python.exe"'], 
    capture_output=True, text=True)
print("=== Python processes ===")
print(result2.stdout)
print(result2.stderr)

result3 = subprocess.run(['powershell', '-Command',
    'tasklist /FI "IMAGENAME eq idf.py*"'], 
    capture_output=True, text=True)
print("=== IDF processes ===")
print(result3.stdout)
print(result3.stderr)
