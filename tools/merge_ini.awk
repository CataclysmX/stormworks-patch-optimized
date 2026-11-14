# Merge the user's stwpatch.ini into the new template:  awk -f merge_ini.awk user.ini template.ini
# The output follows the template (new options, comments, order); every key the user set keeps
# the user's line, also where the template only has it commented out (";threads=6"); user keys
# the template does not know are kept at the end of their section. Same rules as merge_ini.ps1.
function key_of(line,    k) {
    k = line
    sub(/^;/, "", k)
    if (k !~ /^[A-Za-z_][A-Za-z0-9_]*=/) return ""
    sub(/=.*/, "", k)
    return k
}
function flush(sec,    i) {                       # user keys of `sec` the template does not have
    for (i = 1; i <= n_user; i++)
        if (user_sec[i] == sec && !used[i]) { print user_line[i]; used[i] = 1 }
}
{ sub(/\r$/, "") }
FNR == NR {                                        # first file: the user's ini
    if ($0 ~ /^\[.*\]$/) { usec = $0; next }
    if ($0 ~ /^[A-Za-z_][A-Za-z0-9_]*=/) {
        n_user++
        user_sec[n_user] = usec
        user_line[n_user] = $0
        idx[usec SUBSEP key_of($0)] = n_user
    }
    next
}
/^\[.*\]$/ { flush(sec); sec = $0; seen[sec] = 1; print; next }
{
    k = key_of($0)
    if (k != "" && ((sec SUBSEP k) in idx) && !used[idx[sec SUBSEP k]]) {
        i = idx[sec SUBSEP k]
        print user_line[i]
        used[i] = 1
        next
    }
    print
}
END {
    flush(sec)
    for (i = 1; i <= n_user; i++)                  # sections the template no longer has
        if (!used[i]) {
            if (!(user_sec[i] in printed)) { print ""; print user_sec[i]; printed[user_sec[i]] = 1 }
            print user_line[i]
        }
}
