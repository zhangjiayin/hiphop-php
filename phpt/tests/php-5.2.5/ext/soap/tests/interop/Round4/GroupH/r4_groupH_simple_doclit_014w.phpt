--TEST--
SOAP Interop Round4 GroupH Simple Doc Lit 014 (php/wsdl): echoMultipleFaults3(3)
--SKIPIF--
<?php require_once('skipif.inc'); ?>
--FILE--
<?php
$client = new SoapClient(dirname(__FILE__)."/round4_groupH_simple_doclit.wsdl",array("trace"=>1,"exceptions"=>0));
$client->echoMultipleFaults3(array("whichFault" => 3,
                                   "param1" => "arg1",
                                   "param2" => "arg2"));
echo $client->__getlastrequest();
$HTTP_RAW_POST_DATA = $client->__getlastrequest();
include("round4_groupH_simple_doclit.inc");
echo "ok\n";
?>
--EXPECT--
<?xml version="1.0" encoding="UTF-8"?>
<SOAP-ENV:Envelope xmlns:SOAP-ENV="http://schemas.xmlsoap.org/soap/envelope/" xmlns:ns1="http://soapinterop.org/types/requestresponse"><SOAP-ENV:Body><ns1:echoMultipleFaults3Request><ns1:whichFault>3</ns1:whichFault><ns1:param1>arg1</ns1:param1><ns1:param2>arg2</ns1:param2></ns1:echoMultipleFaults3Request></SOAP-ENV:Body></SOAP-ENV:Envelope>
<?xml version="1.0" encoding="UTF-8"?>
<SOAP-ENV:Envelope xmlns:SOAP-ENV="http://schemas.xmlsoap.org/soap/envelope/" xmlns:ns1="http://soapinterop.org/types/part"><SOAP-ENV:Body><SOAP-ENV:Fault><faultcode>SOAP-ENV:Server</faultcode><faultstring>Fault in response to 'echoMultipleFaults3'.</faultstring><detail><ns1:StringPart>arg1</ns1:StringPart></detail></SOAP-ENV:Fault></SOAP-ENV:Body></SOAP-ENV:Envelope>
ok
