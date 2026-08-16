#!/system/bin/sh

# Calibrate BootMenu's ticking-but-unsynchronised Qualcomm RTC against
# Android's current local time.  Only a seconds-of-day offset is persisted;
# the service refreshes it after every Android boot and therefore follows the
# system timezone and automatic time corrections without changing the RTC.
PERSIST_MNT=/mnt/vendor/persist
EFISP_DIR=$PERSIST_MNT/efisp
RTC_TIME=/sys/class/rtc/rtc0/time
ONCE=$1

attempt=0
while true; do
  if grep -q " $PERSIST_MNT " /proc/mounts 2>/dev/null && [ -r "$RTC_TIME" ]; then
    android_year=$(date +%Y 2>/dev/null)
    android_time=$(date +%H:%M:%S 2>/dev/null)
    rtc_time=$(cat "$RTC_TIME" 2>/dev/null)
    case "$android_year:$android_time:$rtc_time" in
      20[2-9][0-9]:[0-2][0-9]:[0-5][0-9]:[0-5][0-9]:[0-2][0-9]:[0-5][0-9]:[0-5][0-9])
        old_ifs=$IFS
        IFS=:
        set -- $android_time $rtc_time
        IFS=$old_ifs
        android_seconds=$((10#$1 * 3600 + 10#$2 * 60 + 10#$3))
        rtc_seconds=$((10#$4 * 3600 + 10#$5 * 60 + 10#$6))
        offset=$((android_seconds - rtc_seconds))
        [ "$offset" -lt 0 ] && offset=$((offset + 86400))
        mkdir -p "$EFISP_DIR" || exit 1
        desired="SFCLOCK1 $offset"
        current=$(cat "$EFISP_DIR/CLOCKOFFSET" 2>/dev/null)
        if [ "$current" != "$desired" ]; then
          tmp="$EFISP_DIR/.CLOCKOFFSET.tmp"
          if printf '%s\n' "$desired" > "$tmp"; then
            chmod 0644 "$tmp"
            mv -f "$tmp" "$EFISP_DIR/CLOCKOFFSET"
            sync
          fi
        fi
        if [ "$ONCE" = "--once" ]; then
          exit 0
        fi
        attempt=120
        ;;
    esac
  fi
  attempt=$((attempt + 1))
  if [ "$attempt" -lt 120 ]; then
    sleep 1
  else
    # Once Android time is valid, wake only occasionally.  Rewriting is
    # skipped unless timezone/time calibration actually changes.
    sleep 60
  fi
done
