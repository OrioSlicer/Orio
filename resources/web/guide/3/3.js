// Orio : plus de programme d'amélioration (télémétrie retirée). Page de passage : elle marque le
// guide comme fait (message user_private_choice), puis demande le profil pour aller au choix des imprimantes.
function OnInit()
{
	SendGuideMessage("user_private_choice");
	SendGuideMessage("request_userguide_profile");
}

function SendGuideMessage( strCmd )
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']=strCmd;

	SendWXMessage( JSON.stringify(tSend) );
}

function HandleStudio( pVal )
{
	let strCmd=pVal['command'];

	if(strCmd=='response_userguide_profile')
	{
		HandleModelInfo(pVal['response']);
	}
}

function HandleModelInfo( pVal )
{
	let Modellist=pVal["model"];
	let nModel=Modellist.length;

	if(nModel==1)
	{
		window.location.href="../22/index.html";
		return;
	}

	window.location.href="../21/index.html";
}
