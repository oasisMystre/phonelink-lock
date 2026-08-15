$code = @"
using System;
using System.IO;
using System.Diagnostics;
using System.Reflection;

[assembly: AssemblyTitle("PhoneLinkLock Setup")]

class Program {
    static void Main(string[] args) {
        try {
            string destDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PhoneLinkLock");
            Directory.CreateDirectory(destDir);
            string destFile = Path.Combine(destDir, "PhoneLinkLock.exe");
            
            using (Stream resStream = Assembly.GetExecutingAssembly().GetManifestResourceStream("PhoneLinkLock.exe")) {
                if (resStream != null) {
                    using (FileStream fs = new FileStream(destFile, FileMode.Create, FileAccess.Write)) {
                        resStream.CopyTo(fs);
                    }
                }
            }
            
            Process.Start(new ProcessStartInfo {
                FileName = destFile,
                UseShellExecute = true
            });
        } catch { }
    }
}
"@

Set-Content -Path "Setup.cs" -Value $code -Encoding UTF8

$csc = (Get-ChildItem -Path "$env:windir\Microsoft.NET\Framework64" -Filter "csc.exe" -Recurse | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
if (-not $csc) {
    throw "Could not find csc.exe"
}

Write-Host "Compiling silent installer using $csc..."
$cmd = "& `"$csc`" /nowarn:1668 /nologo /target:winexe /out:`"build\PhoneLinkLock_Setup.exe`" /res:`"build\Release\PhoneLinkLock.exe`" Setup.cs"
Invoke-Expression $cmd
if ($LASTEXITCODE -eq 0) {
    Write-Host "Successfully built build\PhoneLinkLock_Setup.exe"
} else {
    Write-Host "Failed to build setup executable."
}
