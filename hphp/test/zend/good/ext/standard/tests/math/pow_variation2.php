<?hh
/* Prototype  : number pow  ( number $base  , number $exp  )
 * Description: Exponential expression.
 * Source code: ext/standard/math.c
 */

// get a class
class classA
{
}
<<__EntryPoint>> function main(): void {
echo "*** Testing pow() : usage variations ***\n";


// heredoc string
$heredoc = <<<EOT
abc
xyz
EOT;

// get a resource variable
$fp = fopen(__FILE__, "r");

$inputs = varray[
       // int data
/*1*/  0,
       1,
       12345,
       -2345,
       2147483647,

       // float data
/*6*/  2.5,
       -2.5,
       12.3456789000e10,
       12.3456789000E-10,
       .5,

       // null data
/*11*/ NULL,
       null,

       // boolean data
/*13*/ true,
       false,
       TRUE,
       FALSE,

       // empty data
/*17*/ "",
       '',
       varray[],

       // string data
/*20*/ "abcxyz",
       'abcxyz',
       $heredoc,

       // object data
/*23*/ new classA(),



       // resource variable
/*24*/ $fp
];

// loop through each element of $inputs to check the behaviour of pow()
$iterator = 1;
foreach($inputs as $input) {
    echo "\n-- Iteration $iterator --\n";
    var_dump(pow(20.3, $input));
    $iterator++;
};
fclose($fp);
echo "===Done===";
}
