<?php
/*
	postfix.inc
	part of the Postfix package for pfSense
	Copyright (C) 2010 Erik Fonnesbeck
	Copyright (C) 2011-2013 Marcello Coutinho

	All rights reserved.

	Redistribution and use in source and binary forms, with or without
	modification, are permitted provided that the following conditions are met:

	1. Redistributions of source code MUST retain the above copyright notice,
	   this list of conditions and the following disclaimer.

	2. Redistributions in binary form must reproduce the above copyright
	   notice, this list of conditions and the following disclaimer in the
	   documentation and/or other materials provided with the distribution.

	THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
	INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
	AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
	AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
	OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
	SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
	INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
	CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
	ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
	POSSIBILITY OF SUCH DAMAGE.

*/
$shortcut_section = "postfix";
require_once("util.inc");
require_once("functions.inc");
require_once("pkg-utils.inc");
require_once("globals.inc");

$pf_version=substr(trim(file_get_contents("/etc/version")),0,3);
if ($pf_version > 2.0)
	define('POSTFIX_LOCALBASE', '/usr/pbi/postfix-' . php_uname("m"));
else
  define('POSTFIX_LOCALBASE','/usr/local');
  

$uname=posix_uname();
if ($uname['machine']=='amd64')
        ini_set('memory_limit', '250M');

function px_text_area_decode($text){
	return preg_replace('/\r\n/', "\n",base64_decode($text));	
}

function px_get_real_interface_address($iface) {
	global $config;
	$iface = convert_friendly_interface_to_real_interface_name($iface);
	$line = trim(shell_exec("ifconfig $iface | grep inet | grep -v inet6"));
	$postfix_enabled=$config['installedpackages']['postfix']['config'][0]['enable_postfix'];
	list($dummy, $ip, $dummy2, $netmask) = explode(" ", $line);
	return array($ip, long2ip(hexdec($netmask)));
}

function sync_relay_recipients($via_cron="cron"){
	global $config,$g;
	#relay recipients
	if ($config['installedpackages']['postfixrecipients']['config']) {
		$relay_recipients="";
		$relay_ldap_recipients="";
		$ad_export= "/usr/local/bin/adexport.pl";
		$postfix_enabled=$config['installedpackages']['postfix']['config'][0]['enable_postfix'];
		if (is_array($config['installedpackages']['postfixrecipients']['config'])) {
			$relay_ldap_recipients="";
			$postfix_recipients_config=$config['installedpackages']['postfixrecipients']['config'][0];
			 if($postfix_recipients_config['enable_url'] && is_URL($postfix_recipients_config['custom_url'])){
				print "extracting from ".$postfix_recipients_config['custom_url']."...";
				$relay_recipients .= file_get_contents($postfix_recipients_config['custom_url']);
				print "(". count(file($postfix_recipients_config['custom_url'])).")\n";
			}
			if($postfix_recipients_config['custom_recipients'])
				$relay_recipients .= px_text_area_decode($postfix_recipients_config['custom_recipients']);
			if($postfix_recipients_config['enable_ldap']){
				#validate cront job
				if ($via_cron == "gui"){
					#running via pfsense gui, not time for ldap fetch.
					$ldap_recipients= POSTFIX_LOCALBASE. '/etc/postfix/relay_ldap_recipients.txt';
					if (!file_exists($ldap_recipients))
						system('/usr/bin/touch '. $ldap_recipients);
					$relay_ldap_recipients=file_get_contents($ldap_recipients);
					}	
				else{				
					#running via crontab, time to get ldap content.
					$ldap_temp=array();
					foreach ($postfix_recipients_config['row'] as $postfix_ldap) {
						print "extracting from ".$postfix_ldap['dc']."...";
						$filename=POSTFIX_LOCALBASE."/etc/postfix/relay_ldap_recipients.".$postfix_ldap['dc'].".txt";
						exec($ad_export." ".$postfix_ldap['dc']." ".$postfix_ldap['cn']." ".$postfix_ldap['username']." ".$postfix_ldap['password'],$ldap_fetch,$status);
						if ($status == 0){
							#write backup conf for ldap server
							$fp=fopen($filename,"w+");
							foreach($ldap_fetch as $key => $value)
									fwrite($fp,$value."\n");
							fclose($fp);
							}
						else{
							if (file_exists($filename)) {
								#LDAP fetch failed...read backup file.
								print "Restoring backup file for ".$postfix_ldap['dc']."...";
								$ldap_fetch=file($filename);
								}
							else{
								#we never got any info from this server.
								print "There is no backup file for ".$postfix_ldap['dc']."...";
								$ldap_fetch=array();
								}
							}
						$ldap_all = array_merge($ldap_temp,$ldap_fetch);
						$ldap_temp=$ldap_all;
						print "(".count($ldap_fetch).")\n";
						$ldap_fetch=array();
						}
						$ldap_unique=array_unique($ldap_all);
						print "Total ldap recipients:".count($ldap_all)."\tunique:".count($ldap_unique)."\n";
						foreach($ldap_unique as $recipient)
							$relay_ldap_recipients.=($recipient != ""?preg_replace("/\s+/","",$recipient)." OK\n":"");
						
					#save ldap relay recipients
					file_put_contents(POSTFIX_LOCALBASE."/etc/postfix/relay_ldap_recipients.txt",$relay_ldap_recipients, LOCK_EX);
					}
				}
		}
	#save all relay recipients, remove duplicates and reload postfix
	$recipients_file=POSTFIX_LOCALBASE."/etc/postfix/relay_recipients";
	file_put_contents($recipients_file.".unsort",$relay_ldap_recipients."\n".$relay_recipients, LOCK_EX);
	exec('/usr/bin/sort -u '.$recipients_file.'.unsort > '.$recipients_file);
	unlink_if_exists($recipients_file.'.unsort');
	exec(POSTFIX_LOCALBASE."/sbin/postmap ".POSTFIX_LOCALBASE."/etc/postfix/relay_recipients");
	mwexec("/usr/local/sbin/postfix reload");
	}
	if($relay_recipients !="" || $relay_ldap_recipients!="")
		return("relay_recipient_maps = hash:".POSTFIX_LOCALBASE."/etc/postfix/relay_recipients\n");
		
}
function check_cron(){
	global $config, $g;
			#check crontab 
			$new_cron=array();
			$cron_cmd_sqlite = "";
			$cron_postfix_sqlite="";
			$cron_cmd= "/usr/local/bin/php -q /usr/local/www/postfix_recipients.php";
			$postfix_enabled=$config['installedpackages']['postfix']['config'][0]['enable_postfix'];
			#check ldap update
			if (is_array($config['installedpackages']['postfixrecipients']['config']))
				$postfix_recipients_config=$config['installedpackages']['postfixrecipients']['config'][0];
			if(preg_match("/(\d+)(\w)/",$postfix_recipients_config['freq'],$matches)){
					$cron_postfix=array("minute" =>	"*",
										"hour"	 =>	"*",
										"mday" 	 =>	"*",
										"month"  =>	"*",
										"wday"	 =>	"*",
										"who"	 =>	"root",
										"command"=>	$cron_cmd);
					switch ($matches[2]){
						case m:
							$cron_postfix["minute"]="*/".$matches[1];
							break;
						case h:
							$cron_postfix["minute"]="0";
							$cron_postfix["hour"]="*/".$matches[1];
							break;
						case d:
							$cron_postfix["minute"]="0";
							$cron_postfix["hour"]="0";
							$cron_postfix["mday"]="*/".$matches[1];
							break;
						default:
							$input_errors[] = "A valid number with a time reference is required for the field 'Frequency'";
					}
			}
			#check crontab Sqlite databases
			if (is_array($config['installedpackages']['postfix']['config']) && $postfix_enabled=="on"){
				$cron_sqlite_queue=$config['installedpackages']['postfix']['config'][0]['update_sqlite'];
				$cron_cmd_sqlite="/usr/local/bin/php -q /usr/local/www/postfix.php";
				if ($cron_sqlite_queue != "" && $cron_sqlite_queue != "never"){
					$cron_postfix_sqlite=array("minute" =>	"*",
											"hour"	 =>	"*",
											"mday" 	 =>	"*",
											"month"  =>	"*",
											"wday"	 =>	"*",
											"who"	 =>	"root",
											"command"=>	"");
					switch ($cron_sqlite_queue){
						case '01min':
							$cron_postfix_sqlite["command"] = $cron_cmd_sqlite ." 01min";
							break;
						case '10min':
							$cron_postfix_sqlite["minute"]="*/10";
							$cron_postfix_sqlite["command"] = $cron_cmd_sqlite ." 10min";
							break;
						case '01hour':
							$cron_postfix_sqlite["minute"]="0";
							$cron_postfix_sqlite["command"] = $cron_cmd_sqlite ." 01hour";
							break;
						case '24hours':
							$cron_postfix_sqlite["minute"]="0";
							$cron_postfix_sqlite["hour"]="0";
							$cron_postfix_sqlite["command"] = $cron_cmd_sqlite ." 24hours";
							break;
						}
				}
			}
			
			#check crontab relay recipients
			$cron_found="";
			if (is_array($config['cron']['item'])){
				#print "<pre>";
				foreach($config['cron']['item'] as $cron){
					#check valid_recipients cron
					if ($cron["command"] == $cron_cmd){
						#postfix cron cmd found
						if($postfix_enabled=="on"){
							$cron_found=$cron;
							if($postfix_recipients_config['enable_ldap'] || $postfix_recipients_config['enable_url']){
								#update cron schedule
								$new_cron['item'][]=$cron_postfix;
								}
							}
						}
					#check sqlite update queue	
					else if(!preg_match("/.usr.local.www.postfix.php/",$cron["command"])){
						#keep all non postfix cron cmds if not empty
						if ($cron["command"] != "")
							$new_cron['item'][]=$cron;
						}
					}
			$write_cron=1;
			# Check if crontab must be changed to valid recipients cmd
			if ($postfix_recipients_config['enable_ldap'] || $postfix_recipients_config['enable_url']){
				if ($cron_found!=$cron_postfix){
					#update postfix cron schedule	
					if (! is_array($cron_found) && $postfix_enabled=="on")
						$new_cron['item'][]=$cron_postfix;
					$write_cron=1;
					}
				}
			else{
				if (is_array($cron_found)){
					#remove postfix cron cmd
					$write_cron=1;
					}
				}
			#check if cron must be changed to Sqlite cmd
			if($cron_sqlite_queue != "" && $cron_sqlite_queue != "never"){
				$new_cron['item'][]=$cron_postfix_sqlite;
				$config['cron']=$new_cron;
				$write_cron=1;
				}
			}

			#call cron functions	
			if ($write_cron==1){
				$config['cron']=$new_cron;
				write_config('Postfix - sync remote sqlite database',$backup = false);
				configure_cron();
				}
			#remove postfix old cron call
			$old_cron=0;
			$crontab = file('/etc/crontab');
			$new_crontab="";
			foreach ($crontab as $line){
				if (preg_match("/php..usr.local.www.postfix_recipients.php/",$line))
					$old_cron=1;
				else
					$new_crontab .= $line;
				}
			if ($old_cron==1)	
				file_put_contents("/etc/crontab",$new_crontab, LOCK_EX);
		
			#print "<pre>". var_dump($new_cron). var_dump($cron_postfix_sqlite).var_dump($config['cron']);
			#exit;
			
		
}
function sync_package_postfix($via_rpc="no") {
	global $config;
	
	log_error("sync_package_postfix called with via_rpc={$via_rpc}");
	# detect boot process
	if (is_array($_POST)){
		if (preg_match("/\w+/",$_POST['__csrf_magic']))
			unset($boot_process);
		else
			$boot_process="on";
	}
		
	if(is_process_running("master") && isset($boot_process) && $via_rpc=="no")
		return;
		
	#check patch in /etc/inc/config.
	$relay_domains = "";
	$transport = "";
	$postfix_config=$config['installedpackages']['postfix']['config'][0];
	if (is_array($config['installedpackages']['postfixdomains']))
		$postfix_domains=$config['installedpackages']['postfixdomains']['config'][0];
	$message_size_limit=($postfix_config['message_size_limit']?$postfix_config['message_size_limit']:"10240000");
	$process_limit=($postfix_config['process_limit']?$postfix_config['process_limit']:"100");
	if (is_array($postfix_domains['row'])) {
		foreach ($postfix_domains['row'] as $postfix_row) {
			$relay_domains .= ' ' . $postfix_row['domain'];
			if (!empty($postfix_row['mailserverip']))
				$transport .= $postfix_row['domain'] . " smtp:[" . $postfix_row['mailserverip'] . "]\n";
				}
			}
	#check cron
	check_cron();
	#check logging
	if ($postfix_config['log_to']){
			switch($postfix_config['log_to']){
				case 'maillog':
					system("/usr/bin/touch /var/log/maillog");
					$mail_syslog="mail.crit;";
				break;
				case 'none':
					$mail_syslog="mail.crit;";
				break;
				default:
					$mail_syslog='mail.*;';
				break;
			}
            #update /etc/inc/system.inc
            $sys_log_file='/etc/inc/system.inc';
            $pfsense_version=preg_replace("/\s/","",file_get_contents("/etc/version"));
            $sys_log = file($sys_log_file);
            $new_sys_log="";
            $found_mail=0;
            foreach ($sys_log as $line){
            	$new_line=preg_replace('/mail.(.|crit);/',$mail_syslog,$line);
            	if (preg_match('/mail.*system.log/',$line) && $postfix_config['log_to'] =="maillog"){
            		$new_sys_log .= 'mail.*'."\t\t\t\t\t\t".'/var/log/maillog'."\n";
            	}
            	if (preg_match('/maillog/',$line)){
            		$new_line ="";
            	}
            	$new_sys_log .= $new_line;
            }
            if (!file_exists('/root/'.$pfsense_version.'.system.inc.backup')) {
            	copy ($sys_log_file,'/root/'.$pfsense_version.'.system.inc.backup');
            }
            file_put_contents($sys_log_file,$new_sys_log, LOCK_EX);
            #mwexec('/usr/local/bin/php -q /usr/local/www/postfix_syslog.php');
            #restart syslog daemon	
            system_syslogd_start();
		}
           
	#check_debug
	if($postfix_config['debug_list'] && $postfix_config['debug_list']!=""){
	$check_debug ="\n#Debugging postfix\n";
	$check_debug.="debug_peer_list = ".px_text_area_decode($postfix_config['debug_list'])."\n";
	$check_debug.="debug_peer_level = ".$postfix_config['debug_level']."\n\n";
	}
	#check relay recipients
	$all_relay_recipients=sync_relay_recipients('gui');
	
	$copyright=<<<ABOUT
#Part of the Postfix package for pfSense
#Copyright (C) 2010 Erik Fonnesbeck
#Copyright (C) 2011-2013 Marcello Coutinho
#All rights reserved.
#DO NOT EDIT THIS FILE


ABOUT;
$pf_dir=POSTFIX_LOCALBASE;
	$postfix_main=<<<EOF
#main.cf\
{$copyright}
mynetworks = {$pf_dir}/etc/postfix/mynetwork_table
mynetworks_style = host
access_map_reject_code= 554
access_map_defer_code = 451
unverified_recipient_reject_code = 550
unknown_client_reject_code = 550
unknown_hostname_reject_code = 550

EOF;
	#Header Maps
	$mynetworks = "";
	if ($config['installedpackages']['postfixacl']['config'][0]['mynetworks']){
		$mynetworks = px_text_area_decode($config['installedpackages']['postfixacl']['config'][0]['mynetworks']);
		}
	#Header Maps
	if ($config['installedpackages']['postfixacl']['config'][0]['header_maps']){
		$postfix_main .= "header_checks = pcre:".POSTFIX_LOCALBASE."/etc/postfix/header_check\n";
		$postfix_main .= "header_size_limit = 1024000\n";
		$header_check = px_text_area_decode($config['installedpackages']['postfixacl']['config'][0]['header_maps']);
		}
	#Helo Maps
	if ($config['installedpackages']['postfixacl']['config'][0]['helo_maps']){
		$helo_check = px_text_area_decode($config['installedpackages']['postfixacl']['config'][0]['helo_maps']);
		}
	#Sender access
	if ($config['installedpackages']['postfixacl']['config'][0]['sender_access']){
		$sender_access = px_text_area_decode($config['installedpackages']['postfixacl']['config'][0]['sender_access']);
		}
	#MIME Maps
	if ($config['installedpackages']['postfixacl']['config'][0]['mime_maps']){
		$postfix_main .= "mime_header_checks = pcre:".POSTFIX_LOCALBASE."/etc/postfix/mime_check\n";
		$mime_check = px_text_area_decode($config['installedpackages']['postfixacl']['config'][0]['mime_maps']);
		}
	#Body Maps
	if ($config['installedpackages']['postfixacl']['config'][0]['body_maps']){
		$postfix_main .= "body_checks = pcre:".POSTFIX_LOCALBASE."/etc/postfix/body_check\n";
		$body_check = px_text_area_decode($config['installedpackages']['postfixacl']['config'][0]['body_maps']);
		}
	#Client CIDR
	if ($config['installedpackages']['postfixacl']['config'][0]['cal_cidr']){
		if ($antispam['zombie_blocker']=='disabled')
			$cal_cidr = px_text_area_decode($config['installedpackages']['postfixacl']['config'][0]['cal_cidr']);
		else
			#formatar o arquivo retirando os 'oks'
			$cal_cidr_tmp = px_text_area_decode($config['installedpackages']['postfixacl']['config'][0]['cal_cidr']); 	
			$cal_cidr = preg_replace('/ ok/i'," permit",$cal_cidr_tmp);
		}
	#Client PCRE
	if ($config['installedpackages']['postfixacl']['config'][0]['cal_pcre']){
		$cal_pcre = px_text_area_decode($config['installedpackages']['postfixacl']['config'][0]['cal_pcre']);
		}
	$postfix_main .= px_text_area_decode($postfix_config['maincf'])."\n". 
		"relay_domains ={$relay_domains}\n" .
		"transport_maps = hash:".POSTFIX_LOCALBASE."/etc/postfix/transport\n" .
		"local_recipient_maps =\n" .
		$all_relay_recipients.
		"mydestination =\n" .
		"mynetworks_style = host\n" .
		"message_size_limit = {$message_size_limit}\n" .
		"default_process_limit = {$process_limit}\n";
	#assign antispam options
	$antispam=$config['installedpackages']['postfixantispam']['config'][0];
	
	if($antispam['antispam_enabled']){
		switch ($antispam['antispam_software']){
			case "mailscanner":
			if ($antispam['hold_mode']=='auto'){
				$header_check .= "\n/^Received:/ HOLD\n";
				$postfix_main_antispam = "#Saving all mail after header/body/rbl/spf checks to mailscanner\n\n";
				}
			break;
			case "policyd2":
			if ($antispam['antispam_location']){
			$postfix_main_antispam = <<<EOF
#using policyd v2
client_throttle = check_policy_service {$antispam['antispam_location']}
smtpd_client_restrictions = check_policy_service {$antispam['antispam_location']}
smtpd_restriction_classes =
		has_our_domain_as_sender
		client_throttle
smtpd_end_of_data_restrictions = check_policy_service {$antispam['antispam_location']}	


EOF;
			}
			else{
			$postfix_main_antispam = "Policyd v2 has no location set.\n\n";	
			}
			break;
		}	
	}
	if ($antispam['reject_unknown_helo_hostname']){
		$reject_unknown_helo_hostname = <<<EOF
smtpd_helo_restrictions = check_helo_access pcre:{$pf_dir}/etc/postfix/helo_check,
				reject_unknown_helo_hostname,
				reject_invalid_helo_hostname,
				reject_non_fqdn_helo_hostname,
				permit
				
EOF;
		}
	if ($antispam['header_check'] == "strong")
	{
	$postfix_main .= <<<EOF
disable_vrfy_command = yes
strict_rfc821_envelopes = yes

#Just reject after helo,sender,client,recipient tests
smtpd_delay_reject = yes

# Don't talk to mail systems that don't know their own hostname.
smtpd_helo_required = yes
{$reject_unknown_helo_hostname}

smtpd_sender_restrictions = reject_non_fqdn_sender,
				reject_unknown_sender_domain,
				reject_unauth_pipelining,
				reject_multi_recipient_bounce,
				permit

# Allow connections from specified local clients and strong check everybody else.								
smtpd_client_restrictions = permit_mynetworks,
				reject_unauth_destination,
				check_client_access pcre:{$pf_dir}/etc/postfix/cal_pcre,
				check_client_access cidr:{$pf_dir}/etc/postfix/cal_cidr,
				reject_unknown_client_hostname,
				reject_unauth_pipelining,
				reject_multi_recipient_bounce,
				permit							

smtpd_recipient_restrictions = permit_mynetworks,
				reject_unauth_destination,
				reject_unauth_pipelining,
				check_client_access pcre:{$pf_dir}/etc/postfix/cal_pcre,
				check_client_access cidr:{$pf_dir}/etc/postfix/cal_cidr,
				check_sender_access hash:{$pf_dir}/etc/postfix/sender_access,
				reject_non_fqdn_helo_hostname,
				reject_unknown_recipient_domain,
				reject_non_fqdn_recipient,
				reject_multi_recipient_bounce,
				reject_unverified_recipient,
				SPFSPFSPFRBLRBLRBL

EOF;
	}
else
	{
	$postfix_main .= <<<EOF
#Just reject after helo,sender,client,recipient tests
smtpd_delay_reject = yes
	
# Don't talk to mail systems that don't know their own hostname.
smtpd_helo_required = yes
{$reject_unknown_helo_hostname}

smtpd_sender_restrictions = reject_unknown_sender_domain,
				RBLRBLRBL							

# Allow connections from specified local clients and rbl check everybody else if rbl check are set.
smtpd_client_restrictions = permit_mynetworks,
				reject_unauth_destination,
				check_sender_access hash:{$pf_dir}/etc/postfix/sender_access,
				check_client_access pcre:{$pf_dir}/etc/postfix/cal_pcre,
				check_client_access cidr:{$pf_dir}/etc/postfix/cal_cidr
				RBLRBLRBL

# Whitelisting: local clients may specify any destination domain.
#,
smtpd_recipient_restrictions = permit_mynetworks, 
				reject_unauth_destination,
				check_sender_access hash:{$pf_dir}/etc/postfix/sender_access,
				check_client_access pcre:{$pf_dir}/etc/postfix/cal_pcre,
				check_client_access cidr:{$pf_dir}/etc/postfix/cal_cidr,
				SPFSPFSPFRBLRBLRBL

EOF;
	}
#check spf option
switch($antispam['postfix_spf']){
	case 'spf_mark_only':
		$postfix_main.= "spf_mark_only = yes\n";
		$spf="reject_spf_invalid_sender,\n\t\t\t\t";
		break;
	case 'disable':
		$spf="";
		break;
	default:
		$spf=$antispam['postfix_spf'].",\n\t\t\t\t";
		break;
}
$postfix_main=preg_replace("/SPFSPFSPF/",$spf,$postfix_main);
$postfix_main .= $postfix_main_antispam.$check_debug;
switch ($antispam['zombie_blocker'])
		{
		case "enforce":
		case "drop":
		case "ignore":
		$postscreen=1;
		break;
		
		case "disabled":
		$postscreen=0;
		break;
		}
	if ($antispam['soft_bounce'] == "enabled")
	{
	$postfix_main.="soft_bounce = yes\n";	
	}
	
	if ($postscreen==1)	#Postscreen enabled
	{
	if(preg_match("/(\d+),(\d+)(s|m|h|w)/",$antispam['greet_time'],$greet)){
			$postfix_main.='postscreen_greet_wait = ${stress?'.$greet[1].'}${stress:'.$greet[2].'}'.$greet[3]."\n";
			}
		$ag=$antispam['after_greeting'];
		if(preg_match("/postscreen_disable_vrfy_command/",$antispam['after_greeting'])){
			$postfix_main.="postscreen_disable_vrfy_command = yes\n";
			}
		if(preg_match("/postscreen_non_smtp_command_enable/",$antispam['after_greeting'])){
			$postfix_main.="postscreen_non_smtp_command_enable = yes\n";
			$postfix_main.="postscreen_non_smtp_command_action = ".$antispam['zombie_blocker']."\n";
			}
		if(preg_match("/postscreen_pipelining_enable/",$antispam['after_greeting'])){
			$postfix_main.="postscreen_pipelining_enable = yes\n";
			$postfix_main.="postscreen_pipelining_action = ".$antispam['zombie_blocker']."\n";
			}
		if(preg_match("/postscreen_bare_newline_enable/",$antispam['after_greeting'])){
			$postfix_main.="postscreen_bare_newline_enable = yes\n";
			$postfix_main.="postscreen_bare_newline_action = ".$antispam['zombie_blocker']."\n";
			}
		if(preg_match("/postscreen_greet_check/",$antispam['after_greeting'])){
			$postfix_main.="postscreen_greet_action = ".$antispam['zombie_blocker']."\n";
			}
	
	$postfix_main.="postscreen_access_list = permit_mynetworks,\n\t\t\tcidr:".POSTFIX_LOCALBASE."/etc/postfix/cal_cidr\n";
	$postfix_main.="postscreen_dnsbl_action= ".$antispam['zombie_blocker']."\n";
	$postfix_main.="postscreen_blacklist_action= ".$antispam['zombie_blocker']."\n";

	#postscreen interface loop
	$ifaces = ($postfix_config['enabled_interface'] ? $postfix_config['enabled_interface'] : 'wan');
	$real_ifaces = array();
	$postfix_master="";
	foreach (explode(",", $ifaces) as $i => $iface) {
		$real_ifaces[] = px_get_real_interface_address($iface);
		if($real_ifaces[$i][0]) {
			$postfix_master .=$real_ifaces[$i][0].":25	inet  n       -       n       -       1       postscreen\n\t-o user=postfix\n";
			$postfix_master .=($antispam['soft_bounce'] == "postscreen"?"\t-o soft_bounce=yes\n":"");
		}
	}
	$postfix_master .= $postfix_inets.<<<MASTEREOF
smtpd     pass  -       -       n       -       -       smtpd
dnsblog   unix  -       -       n       -       0       dnsblog
tlsproxy  unix  -       -       n       -       0       tlsproxy

MASTEREOF;
	$rbl2="";
	if ($antispam['rbl_servers'] != "")
		{
		$postfix_main .= "postscreen_dnsbl_sites=" . $antispam['rbl_servers']."\n";
		$postfix_main .= "postscreen_dnsbl_threshold=" . $antispam['rbl_threshold']."\n";
		}
	}
	else
	{					#Postscreen disabled
	if ($antispam['rbl_servers'] != "")
		{
		$RBL = explode(",",$antispam['rbl_servers']);
		foreach ($RBL as $rbl)
			{
			$prefix=($rbl2 !=""?"\t\t\t\t":"");
			$rbl2.= $prefix."reject_rbl_client $rbl,\n";
			}
		}
	
	#interface loop
	$postfix_inets="";
	$ifaces = ($postfix_config['enabled_interface'] ? $postfix_config['enabled_interface'] : 'loopback');
	$real_ifaces = array();
	$postfix_master="";
	foreach (explode(",", $ifaces) as $i => $iface) {
		$real_ifaces[] = px_get_real_interface_address($iface);
		if($real_ifaces[$i][0]) {
			$postfix_master .=$real_ifaces[$i][0].":25	inet  n       -       n       -       -       smtpd\n";
		}
	}
		
	}
	$rbl2.=($rbl2 !=""?"\t\t\t\tpermit\n":"permit\n");
	$postfix_main=preg_replace("/RBLRBLRBL/",$rbl2,$postfix_main);
	
	#Header Maps
	$anvil_config=$config['installedpackages']['postfixantispam']['config'][0]['anvil'];
	if ($anvil_config =='enabled' || ($anvil_config =='postscreen' && $postscreen==1))
		$anvil='anvil     unix  -       -       n       -       1       anvil';
	
	$postfix_master .= <<<MASTEREOF2
pickup    fifo  n       -       n       60      1       pickup
cleanup   unix  n       -       n       -       0       cleanup
qmgr      fifo  n       -       n       300     1       qmgr
tlsmgr    unix  -       -       n       1000?   1       tlsmgr
rewrite   unix  -       -       n       -       -       trivial-rewrite
bounce    unix  -       -       n       -       0       bounce
defer     unix  -       -       n       -       0       bounce
trace     unix  -       -       n       -       0       bounce
verify    unix  -       -       n       -       1       verify
flush     unix  n       -       n       1000?   0       flush
proxymap  unix  -       -       n       -       -       proxymap
proxywrite unix -       -       n       -       1       proxymap
smtp      unix  -       -       n       -       -       smtp
relay     unix  -       -       n       -       -       smtp
	-o smtp_fallback_relay=
showq     unix  n       -       n       -       -       showq
error     unix  -       -       n       -       -       error
retry     unix  -       -       n       -       -       error
discard   unix  -       -       n       -       -       discard
local     unix  -       n       n       -       -       local
virtual   unix  -       n       n       -       -       virtual
lmtp      unix  -       -       n       -       -       lmtp
scache    unix  -       -       n       -       1       scache
{$anvil}

MASTEREOF2;
	
	conf_mount_rw();

	log_error("Writing out configuration");
	file_put_contents(POSTFIX_LOCALBASE."/etc/postfix/main.cf", $postfix_main, LOCK_EX);
	file_put_contents(POSTFIX_LOCALBASE."/etc/postfix/master.cf", $postfix_master, LOCK_EX);
	file_put_contents(POSTFIX_LOCALBASE."/etc/postfix/transport", $transport, LOCK_EX);
	file_put_contents(POSTFIX_LOCALBASE."/etc/postfix/sender_access", $sender_access, LOCK_EX);
	file_put_contents(POSTFIX_LOCALBASE."/etc/postfix/cal_cidr", $cal_cidr, LOCK_EX);
	file_put_contents(POSTFIX_LOCALBASE."/etc/postfix/cal_pcre", $cal_pcre, LOCK_EX);
	file_put_contents(POSTFIX_LOCALBASE."/etc/postfix/header_check", $header_check, LOCK_EX);
	file_put_contents(POSTFIX_LOCALBASE."/etc/postfix/helo_check", $helo_check, LOCK_EX);
	file_put_contents(POSTFIX_LOCALBASE."/etc/postfix/mime_check", $mime_check, LOCK_EX);
	file_put_contents(POSTFIX_LOCALBASE."/etc/postfix/body_check", $body_check, LOCK_EX);
	file_put_contents(POSTFIX_LOCALBASE."/etc/postfix/mynetwork_table", $mynetworks, LOCK_EX);
	$FILES=array("transport","sender_access");
	foreach ($FILES as $file)
		{
		mwexec(POSTFIX_LOCALBASE."/sbin/postmap ".POSTFIX_LOCALBASE."/etc/postfix/".$file);
		}
	
	#check postix dirs
	$dirs=array("/var/spool/postfix","/etc/mail","/var/db/postfix","/var/mail/postfix");
	foreach ($dirs as $dir)
		if (!is_dir($dir) && !file_exists($dir))
			mkdir($dir, 0755,TRUE);
	
	#check postfix owners
	$dirs=array("/var/db/postfix","/var/mail/postfix");
	foreach ($dirs as $dir){
			chown($dir, 'postfix');
			chgrp($dir, 'postfix');
			}
	if (!file_exists("/etc/mail/aliases"))
		touch("/etc/mail/aliases");
	exec("/usr/local/bin/newaliases");
	postfix_start();
	
	#Do not sync during boot
	if(!isset($boot_process) || $via_rpc=="yes")
		postfix_sync_on_changes();
		
}
function postfix_start(){
	global $config;
	$pf_dir=POSTFIX_LOCALBASE;
	$start=<<<EOF
	
	sysctl kern.ipc.nmbclusters=65536
	sysctl kern.ipc.somaxconn=16384
	sysctl kern.maxfiles=131072
	sysctl kern.maxfilesperproc=104856
	sysctl kern.threads.max_threads_per_proc=4096
	{$pf_dir}/sbin/postfix start
	
EOF;
	$stop = POSTFIX_LOCALBASE."/sbin/postfix stop\n";
	log_error("Writing rc_file");		
	write_rcfile(array("file" => "postfix.sh", "start" => $start, "stop" => $stop));

	sleep(1);
	if (is_array($config['installedpackages']['postfix']) && $config['installedpackages']['postfix']['config'][0]['enable_postfix']){
		log_error("Reloading/starting postfix");
		system('/bin/chmod +x /usr/local/etc/rc.d/postfix.sh');
		mwexec_bg(POSTFIX_LOCALBASE."/sbin/postfix reload || /usr/local/etc/rc.d/postfix.sh start");
		log_error("Postfix setup completed");
		}
	else{
		log_error("Stopping postfix");
		mwexec("/usr/local/etc/rc.d/postfix.sh stop");
		system('/bin/chmod -x /usr/local/etc/rc.d/postfix.sh');
		}
		
	conf_mount_ro();
}

function postfix_validate_input($post, &$input_errors) {
	foreach ($post as $key => $value) {
		if (empty($value))
			continue;
		if($key == "greet_time" && !preg_match("/(\d+),(\d+)(s|m|h|w)/",$value))
				$input_errors[] = "Wrong greet time sintax.";			
		if($key == "message_size_limit" && !is_numeric($value))
				$input_errors[] = "Message size limit must be numeric.";
		if($key == "process_limit" && !is_numeric($value))
				$input_errors[] = "Process limit must be numeric.";	
		if($key == "freq" && (!preg_match("/^\d+(h|m|d)$/",$value) || $value == 0))
				$input_errors[] = "A valid number with a time reference is required for the field 'Frequency'";
		if (substr($key, 0, 2) == "dc" && !is_hostname($value))
				$input_errors[] = "{$value} is not a valid host name.";
		if (substr($key, 0, 6) == "domain" && is_numeric(substr($key, 6))) {
			if (!is_domain($value))
				$input_errors[] = "{$value} is not a valid domain name.";
		} else if (substr($key, 0, 12) == "mailserverip" && is_numeric(substr($key, 12))) {
			if (empty($post['domain' . substr($key, 12)]))
				$input_errors[] = "Domain for {$value} cannot be blank.";
			if (!is_ipaddr($value) && !is_hostname($value))
				$input_errors[] = "{$value} is not a valid IP address or host name.";
		}
	}
}

function postfix_php_install_command() {
	sync_package_postfix();
}

function postfix_php_deinstall_command() {
	global $config;
	#disable service
	if (is_array($config['installedpackages']['postfix']))
		$config['installedpackages']['postfix']['config'][0]['enable_postfix']="";
	write_config();
	sync_package_postfix();
	conf_mount_rw();
	unlink_if_exists("/usr/local/etc/rc.d/postfix.sh");
	conf_mount_ro();
}

/* Uses XMLRPC to synchronize the changes to a remote node */
function postfix_sync_on_changes() {
	global $config, $g;			
	if (is_array($config['installedpackages']['postfixsync']['config'])){
		$postfix_sync=$config['installedpackages']['postfixsync']['config'][0];
		$synctimeout = $postfix_sync['synctimeout'];
		$synconchanges = $postfix_sync['synconchanges'];
		switch ($synconchanges){
			case "manual":
				if (is_array($postfix_sync[row])){
					$rs=$postfix_sync[row];
					}
				else{
					log_error("[postfix] xmlrpc sync is enabled but there is no hosts to push postfix config.");
					return;
					}
				break;
			case "auto":
					if (is_array($config['installedpackages']['carpsettings']) && is_array($config['installedpackages']['carpsettings']['config'])){
						$system_carp=$config['installedpackages']['carpsettings']['config'][0];
						$rs[0]['ipaddress']=$system_carp['synchronizetoip'];
						$rs[0]['username']=$system_carp['username'];
						$rs[0]['password']=$system_carp['password'];
						$rs[0]['enabless']=true;
						$rs[0]['sync_type']="xmlrpc";
						if (! is_ipaddr($system_carp['synchronizetoip'])){
							log_error("[postfix] xmlrpc sync is enabled but there is no system backup hosts to push postfix config.");
							return;
							}
					}
					else{
						log_error("[postfix] xmlrpc sync is enabled but there is no system backup hosts to push postfix config.");
						return;
					}
				break;			
			default:
				return;
			break;
		}
		if (is_array($rs)){
			log_error("[postfix] xmlrpc sync is starting.");
			foreach($rs as $sh){
				$sync_to_ip = $sh['ipaddress'];
				if($sh['username'])
					$username = $sh['username'];
				else
					$username = 'admin';
				if($sh['password'] && $sh['ipaddress'] && $sh['enabless'])
					postfix_do_xmlrpc_sync($sh['ipaddress'], $username, $sh['password'],$sh['sync_type'],$synctimeout);
				}
			log_error("[postfix] xmlrpc sync is ending.");
			}
 		}
}


/* Do the actual XMLRPC sync */
function postfix_do_xmlrpc_sync($sync_to_ip,$username,$password,$sync_type,$synctimeout) {
	global $config, $g;

	if(!$username)
		$username="admin";
	
	if(!$password)
		return;

	if(!$sync_to_ip)
		return;
		
	if(!$synctimeout)
		$synctimeout=120;

	$xmlrpc_sync_neighbor = $sync_to_ip;
    if($config['system']['webgui']['protocol'] != "") {
		$synchronizetoip = $config['system']['webgui']['protocol'];
		$synchronizetoip .= "://";
    }
    $port = $config['system']['webgui']['port'];
    /* if port is empty lets rely on the protocol selection */
    if($port == "") {
		if($config['system']['webgui']['protocol'] == "http") 
			$port = "80";
		else 
			$port = "443";
    }
	$synchronizetoip .= $sync_to_ip;

	/* xml will hold the sections to sync */
	$xml = array();
	$sync_xml=$config['installedpackages']['postfixsync']['config'][0]['synconchanges'];
	$sync_db=$config['installedpackages']['postfixsync']['config'][0]['rsync'];
	if ($sync_xml && preg_match("/xmlrpc/",$sync_type)){
		log_error("Include postfix xmls");
		$xml['postfix'] = $config['installedpackages']['postfix'];
		$xml['postfixdomains'] = $config['installedpackages']['postfixdomains'];
		$xml['postfixacl'] = $config['installedpackages']['postfixacl'];
		$xml['postfixrecipients'] = $config['installedpackages']['postfixrecipients'];
		$xml['postfixantispam'] = $config['installedpackages']['postfixantispam'];
		}
	if (count($xml) > 0){
		/* assemble xmlrpc payload */
		$params = array(
			XML_RPC_encode($password),
			XML_RPC_encode($xml)
		);
	
		/* set a few variables needed for sync code borrowed from filter.inc */
		$url = $synchronizetoip;
		log_error("Beginning Postfix XMLRPC sync to {$url}:{$port}.");
		$method = 'pfsense.merge_installedpackages_section_xmlrpc';
		$msg = new XML_RPC_Message($method, $params);
		$cli = new XML_RPC_Client('/xmlrpc.php', $url, $port);
		$cli->setCredentials($username, $password);
		if($g['debug'])
			$cli->setDebug(1);
		/* send our XMLRPC message and timeout after $sync_timeout seconds */
		$resp = $cli->send($msg, $synctimeout);
		if(!$resp) {
			$error = "A communications error occurred while attempting postfix XMLRPC sync with {$url}:{$port}.";
			log_error($error);
			file_notice("sync_settings", $error, "Postfix Settings Sync", "");
		} elseif($resp->faultCode()) {
			$cli->setDebug(1);
			$resp = $cli->send($msg, $synctimeout);
			$error = "An error code was received while attempting postfix XMLRPC sync with {$url}:{$port} - Code " . $resp->faultCode() . ": " . $resp->faultString();
			log_error($error);
			file_notice("sync_settings", $error, "Postfix Settings Sync", "");
		} else {
			log_error("Postfix XMLRPC sync successfully completed with {$url}:{$port}.");
		}
		
		/* tell postfix to reload our settings on the destionation sync host. */
		$method = 'pfsense.exec_php';
		$execcmd  = "require_once('/usr/local/pkg/postfix.inc');\n";
		$execcmd .= "sync_package_postfix('yes');";
		
		/* assemble xmlrpc payload */
		$params = array(
			XML_RPC_encode($password),
			XML_RPC_encode($execcmd)
		);
	
		log_error("postfix XMLRPC reload data {$url}:{$port}.");
		$msg = new XML_RPC_Message($method, $params);
		$cli = new XML_RPC_Client('/xmlrpc.php', $url, $port);
		$cli->setCredentials($username, $password);
		$resp = $cli->send($msg, $synctimeout);
		if(!$resp) {
			$error = "A communications error occurred while attempting postfix XMLRPC sync with {$url}:{$port} (pfsense.exec_php).";
			log_error($error);
			file_notice("sync_settings", $error, "postfix Settings Sync", "");
		} elseif($resp->faultCode()) {
			$cli->setDebug(1);
			$resp = $cli->send($msg, $synctimeout);
			$error = "An error code was received while attempting postfix XMLRPC sync with {$url}:{$port} - Code " . $resp->faultCode() . ": " . $resp->faultString();
			log_error($error);
			file_notice("sync_settings", $error, "postfix Settings Sync", "");
		} else {
			log_error("postfix XMLRPC reload data success with {$url}:{$port} (pfsense.exec_php).");
		}
	}
}

?>
