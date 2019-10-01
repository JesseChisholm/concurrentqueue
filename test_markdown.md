Test Form:
<form action="post" url="" id=form1>
<table border="1">
<tr><th valign="top" align="left">INGEST</th>
<td valign="top" align="left"><input type="text" value="https://ingestjson-dev-dev.in.blizzardgdp.com/data" /></td>
<tr>
<th valign="top" align="left">MESSAGE</th>
<td valign="top" align="left"><input type="text" multiline="true" rows="4" columns="50" value=
{
"context":{},
"payloads":[
    {"package_name":"Blizzard.Telemetry.Standard.Process",
     "message_name":"Start",
     "contents":{"pid":1, "command_line":"test"}
    },
    {"package_name":"Blizzard.Telemetry.Standard.Process",
     "message_name":"Finish",
     "contents":{"pid":1, "exit_code":0}
    }
  ]
}
" /></td>
</tr>
<tr>
<th valign="top" align="left">ACTION</th>
<td valign="top" align="left"><input type="submit" value="Submit" /></td>
</tr>
</table>
</form>
<button type="submit" form="form1" value="Submit" />
: Tested Form
