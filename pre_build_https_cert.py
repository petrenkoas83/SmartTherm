# Pre-build: для env esp32devdeb_https генерирует include/cert_embed.h,
# если файла ещё нет (сертификат для HTTPS, создаётся на хосте при сборке).
Import("env")
import os
import subprocess

pioenv = env.get("PIOENV", "")
if "esp32devdeb_https" in pioenv:
    project_dir = env.get("PROJECT_DIR")
    cert_h = os.path.join(project_dir, "include", "cert_embed.h")
    script = os.path.join(project_dir, "scripts", "generate_https_cert.sh")
    if not os.path.isfile(cert_h):
        print("HTTPS: cert_embed.h not found, running scripts/generate_https_cert.sh ...")
        subprocess.check_call(["bash", script], cwd=project_dir)
    else:
        print("HTTPS: using existing cert_embed.h")
