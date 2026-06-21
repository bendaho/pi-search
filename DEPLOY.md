# Деплой PI SEARCH на VPS — пошаговая инструкция

## 1. Выбор VPS

Подойдёт любой Linux VPS с 2GB+ RAM:

| Провайдер | Минимальный план | Ссылка |
|-----------|-----------------|--------|
| Oracle Cloud | **Free Tier (бесплатно!)** | https://cloud.oracle.com |
| DigitalOcean | $6/mo (1GB RAM, слабовато) | https://digitalocean.com |
| Hetzner | €4.5/mo (2GB RAM) | https://hetzner.com |
| Vultr | $6/mo (1GB RAM) | https://vultr.com |

Рекомендую **Oracle Free Tier** — бесплатно, 4 ядра, 24GB RAM. Или **Hetzner CX22** — €4.5 за 2GB RAM.

Нужен VPS на **Ubuntu 22.04 или 24.04**.

---

## 2. Подключение к серверу

После создания VPS ты получишь:
- IP-адрес (например `185.123.45.67`)
- Пароль root (или SSH-ключ)

Подключись:
```bash
ssh root@185.123.45.67
```

Если SSH-ключ:
```bash
ssh -i ~/.ssh/my_key root@185.123.45.67
```

---

## 3. Установка Docker

Подключился к серверу — выполни:

```bash
# Обновить систему
apt update && apt upgrade -y

# Установить Docker (одна команда)
curl -fsSL https://get.docker.com | sh

# Проверить что Docker работает
docker --version
```

Должно вывести что-то вроде `Docker version 24.x.x`.

---

## 4. Клонирование проекта

```bash
# Клонировать репозиторий
git clone https://github.com/bendaho/pi-search.git

# Перейти в папку
cd pi-search
```

---

## 5. Сборка и запуск

```bash
# Собрать Docker-образ (скачает pi-billion.txt ~1GB, скомпилирует сервер)
# ВНИМАНИЕ: сборка занимает 5-15 минут
docker compose up -d --build
```

Что происходит внутри:
1. Скачивается `pi-billion.txt` (1 млрд знаков Пи) — ~954MB
2. Компилируется C++ сервер с оптимизациями (-O3)
3. Сборка SQLite из амальгамации
4. Запуск сервера на порту 8080

Проверить что запустилось:
```bash
# Посмотреть логи
docker compose logs -f

# Должно выйти:
# Loaded 1000000001 digits in XXXX ms
# Server on http://localhost:8080
```

Нажми `Ctrl+C` чтобы выйти из логов (сервер продолжит работать).

---

## 6. Проверка

Открой в браузере:
```
http://185.123.45.67:8080
```

(Замени на свой IP-адрес)

Должен открыться сайт с тёмным терминальным дизайном. Попробуй поиск — введи `314159`.

---

## 7. Открытие порта (если сайт не открывается)

Если браузер не может подключиться — нужно открыть порт 8080 в файрволе.

### Oracle Cloud:
1. Зайди в Oracle Cloud → Networking → Virtual Cloud Networks
2. Выбери свою VCN → Security Lists → Default Security List
3. Add Ingress Rules:
   - Source CIDR: `0.0.0.0/0`
   - Destination Port Range: `8080`
   - Protocol: TCP

### DigitalOcean / Vultr / Hetzner:
Обычно файрвол открыт по умолчанию. Если нет:
```bash
# UFW (если установлен)
ufw allow 8080/tcp
ufw reload
```

---

## 8. Настройка домена (опционально)

Если хочешь `pi.yourdomain.com` вместо IP:

### Установить Nginx:
```bash
apt install nginx -y
```

### Создать конфиг:
```bash
cat > /etc/nginx/sites-available/pi-search << 'EOF'
server {
    listen 80;
    server_name pi.yourdomain.com;

    location / {
        proxy_pass http://127.0.0.1:8080;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
    }
}
EOF
```

### Активировать:
```bash
ln -s /etc/nginx/sites-available/pi-search /etc/nginx/sites-enabled/
rm /etc/nginx/sites-enabled/default
nginx -t && systemctl reload nginx
```

### DNS-записи:
В настройках домена добавь A-запись:
```
pi.yourdomain.com → 185.123.45.67
```

### SSL-сертификат (https):
```bash
apt install certbot python3-certbot-nginx -y
certbot --nginx -d pi.yourdomain.com
```

После этого сайт будет доступен на `https://pi.yourdomain.com`.

---

## 9. Автозапуск после перезагрузки

Docker Compose уже это делает благодаря `restart: unless-stopped`. Проверим:

```bash
# Перезагрузи сервер
reboot

# После перезагрузки подключись снова и проверь
docker compose ps
# Должно показать: pi-search  running

curl http://localhost:8080/api/stats
# {"pi_digits_loaded":1000000001,"status":"ok"}
```

---

## 10. Управление

```bash
# Остановить
docker compose down

# Запустить
docker compose up -d

# Пересобрать (после обновления кода)
git pull
docker compose up -d --build

# Посмотреть размер образа
docker images

# Логи
docker compose logs -f

# Зайти внутрь контейнера
docker exec -it pi-search-pi-search-1 bash
```

---

## Полезные команды (шпаргалка)

```bash
# Статус контейнера
docker compose ps

# Логи (последние 100 строк)
docker compose logs --tail 100

# Перезапуск
docker compose restart

# Обновить код и пересобрать
cd pi-search && git pull && docker compose up -d --build

# Удалить образ и пересобрать с нуля
docker compose down
docker image rm pi-search-pi-search
docker compose up -d --build

# Мониторинг ресурсов
docker stats
```

---

## Размер и ресурсы

| Что | Размер |
|-----|--------|
| Docker-образ | ~1.2 GB |
| RAM при работе | ~1.1 GB |
| Место на диске | ~2.2 GB |
| Время сборки | 5-15 минут |

VPS с 2GB RAM — достаточно. С 1GB будет работать но впритык.

---

## Возможные проблемы

### "Cannot connect to Docker daemon"
```bash
systemctl start docker
systemctl enable docker
```

### "Port 8080 already in use"
```bash
# Найти что занимает порт
lsof -i :8080
# Убить процесс или сменить порт в docker-compose.yml
```

### Сайт открывается но поиск не работает
Проверь логи:
```bash
docker compose logs -f
```
Должно быть `Loaded 1000000001 digits`. Если нет — не скачался pi-billion.txt.

### Docker build очень долгий
Нормально — скачивание 1GB файла занимает время. Зависит от скорости сервера.
