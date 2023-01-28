res = ''
i = 0
with open('./a.c', 'r', encoding='utf8') as f:
    for line in f.readlines():
        i += 1
        if i < 4900:
            pass
        else:
            res += line

with open('./a.c', 'w', encoding='utf8') as f:
    f.write(res)
# {"watch","Watch the given keys to determine execution of the MULTI/EXEC block","O(1) for every key.","2.2.0",CMD_DOC_NONE,NULL,NULL,COMMAND_GROUP_TRANSACTIONS,WATCH_History,WATCH_tips,watchCommand,-2,CMD_NOSCRIPT | CMD_LOADING | CMD_STALE | CMD_FAST | CMD_ALLOW_BUSY,ACL_CATEGORY_TRANSACTION,{{NULL, 0, KSPEC_BS_INDEX, .bs.index = {1}, KSPEC_FK_RANGE, .fk.range = {-1, 1, 0}}},.args = WATCH_Args},
